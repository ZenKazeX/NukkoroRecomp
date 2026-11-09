#include "gekko/plume_present.h"

#ifdef SHADOW_ENABLE_PLUME

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

#include "gekko/display.h"
#include "gekko/present_shader_cache.h"
#include "gekko/host_jobs.h"
#include "gekko/profiling.h"
#include "gekko/render/presentation_retirement.h"

// plume's public headers use anonymous unions, which /W4 reports as a
// nonstandard extension. They are third-party headers; the project's own
// warning level is unchanged either side of this include block.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4201)
#endif
#include "plume_d3d12.h"
#include "plume_render_interface.h"
#include "plume_render_interface_types.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace gekko {
namespace {

// Direct3D 12 requires the row pitch of a placed footprint to be a multiple of
// 256 bytes. plume derives the pitch from rowWidth in pixels, so the pixel count
// has to round up to a multiple of 64 at four bytes each.
constexpr u32 kPitchAlignmentPixels = 64;

u32 aligned_row_pixels(u32 pixels) {
    return ((pixels + kPitchAlignmentPixels - 1u) / kPitchAlignmentPixels) * kPitchAlignmentPixels;
}

// Presenter-side timing (D06-F5). One QPC read, ~20-25 ns, at most a handful
// per present on the UI thread; never on the guest or Flipper threads.
u64 elapsed_ns_since(std::chrono::steady_clock::time_point start) {
    const auto delta = std::chrono::steady_clock::now() - start;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count();
    return ns > 0 ? static_cast<u64>(ns) : 0u;
}

// Cumulative count behind the "host/present refusals" plot: its SLOPE is the
// refusal rate and the timeline says exactly when. present() and
// present_native() only ever run on the presenting thread, so a plain counter
// is enough. Exists only in a profiling build.
GEKKO_PROFILE_ONLY(u64 g_present_refusals = 0;)

const presentshaders::EmbeddedShader* find_present_shader(const char* entry) {
    for (std::size_t index = 0; index < presentshaders::kEmbeddedShaderCount; ++index) {
        if (std::strcmp(presentshaders::kEmbeddedShaders[index].entry, entry) == 0) {
            return &presentshaders::kEmbeddedShaders[index];
        }
    }
    return nullptr;
}

}  // namespace

struct PlumePresenter::Impl {
    static constexpr int kFlight = 3;

    struct Slot {
        std::unique_ptr<plume::RenderBuffer> upload;
        std::unique_ptr<plume::RenderCommandList> commands;
        std::unique_ptr<plume::RenderCommandFence> fence;
        u64 upload_size = 0;
        u32* mapped = nullptr;
        bool in_flight = false;
        plume::RenderFenceWait retirement_outcome = plume::RenderFenceWait::SIGNALED;
        // The staged surface outside the content rect must be defined. It is
        // cleared once per (surface, content rect) geometry rather than once
        // per present: every present overwrites exactly the content rect, so
        // the letterbox/pillarbox region stays as the last clear left it.
        // Any change to the geometry - or a new upload buffer, or a swap-chain
        // resize - drops this and forces a fresh clear.
        bool cleared = false;
        u32 cleared_surface_width = 0;
        u32 cleared_surface_height = 0;
        u32 cleared_x = 0;
        u32 cleared_y = 0;
        u32 cleared_width = 0;
        u32 cleared_height = 0;
        // GPU blit path: the guest frame at its own size, uploaded once and
        // magnified by the texture unit instead of by the host CPU.
        std::unique_ptr<plume::RenderTexture> source;
        std::unique_ptr<plume::RenderDescriptorSet> source_descriptor;
        u32 source_width = 0;
        u32 source_height = 0;
        // Native XFB pins this flight slot still owes the rasterizer (D06-F7).
        // At most two: the presented generation and the blended predecessor.
        // Only ever touched on the presenting thread.
        PlumePresenter::NativePin pins[2];
        u32 pin_count = 0;
        // When this slot adopted its pins. Pin HOLD TIME is the mechanism
        // behind NativeXFB reserve starvation, and a pin handed back by
        // retire_completed_presents has a very different lifetime from one
        // wait_slot releases three presents later - slots_retired_early only
        // counts the first kind. Written unguarded in a profiling build: a
        // GEKKO_PROFILE_CONNECTED() guard on the WRITE would make every value
        // read after a mid-run attach garbage.
        GEKKO_PROFILE_ONLY(std::chrono::steady_clock::time_point pins_adopted_at;)
    };

    std::unique_ptr<plume::RenderInterface> interface_;
    std::unique_ptr<plume::RenderDevice> owned_device;
    std::unique_ptr<plume::RenderCommandQueue> owned_queue;
    plume::RenderDevice* device = nullptr;
    plume::RenderCommandQueue* queue = nullptr;
    std::mutex* queue_mutex = nullptr;
    std::unique_ptr<plume::RenderSwapChain> swap_chain;
    Slot slots[kFlight];
    int slot = 0;
    std::unique_ptr<plume::RenderBuffer> readback;
    u64 readback_size = 0;
    bool verify = false;
    PlumePresenter::VerifyReport report;
    bool ready = false;
    bool failed = false;
    render::PresentationFailure terminal_failure;
    std::string terminal_error;
    bool vsync = true;
    // GPU presentation blit. The CPU resampler remains as the fallback for any
    // adapter or driver where this fails to build, and is still the path when
    // the frame is already the size of the content rect.
    // Opt-in via --present-gpu-blit while the command list is still being
    // proven. The CPU resampler is the default and is the measured-correct
    // path; this one removes ~0.84 of a core but must not be able to hang the
    // adapter for anyone who has not asked for it.
    bool blit_requested = false;
    // Area resolve at present. Off restores the original single bilinear tap,
    // which is what every shipped build before 2026-08-31 did.
    bool area_resolve = true;
    bool blit_ready = false;
    // One-shot acceptance capture of what actually reached the back buffer.
    // The CPU resampler can be checked with --verify-plume; the blit writes
    // through the GPU, so the only way to confirm its geometry - orientation,
    // offset, extent, filtering - is to read the presented surface back.
    std::string blit_dump_path;
    int blit_dump_countdown = 0;
    std::unique_ptr<plume::RenderPipelineLayout> blit_layout;
    std::unique_ptr<plume::RenderPipeline> blit_pipeline;
    std::unique_ptr<plume::RenderShader> blend_pixel_shader;
    std::unique_ptr<plume::RenderPipeline> blend_pipeline;
    std::unique_ptr<plume::RenderSampler> blit_sampler;
    std::unique_ptr<plume::RenderFramebuffer> blit_framebuffers[kFlight];
    const plume::RenderTexture* blit_framebuffer_target[kFlight] = {};

    // ---- 2026-09-02 presenter switches and counters ----------------------
    // All three are host-side only: the presenter consumes finished GPU
    // textures and has no route into Cpu/System, so nothing here can be
    // observed by the guest (docs/TIMING_ARCHITECTURE.md:125-151).
    // D06-F6: hold the shared presentation queue mutex only around
    // executeCommandLists, as the CPU-upload path always has.
    bool narrow_queue_lock = true;
    // D06-F7: default OFF - see the header for why the safe order is not the
    // default one.
    bool deferred_pin_release = true;
    PlumePresenter::NativeReleaseHook native_release_hook = nullptr;
    void* native_release_context = nullptr;
    // D06-F5. Written and read on the presenting thread only.
    PlumePresenter::PresentStats stats;
    // Live bytes of the GPU staging this file owns: the per-slot upload
    // buffers, the per-slot blit source textures and the readback buffer.
    // Mirrors the GEKKO_PROFILE_ALLOC/FREE pairs so the plot and the pool
    // cannot disagree.
    GEKKO_PROFILE_ONLY(u64 staging_bytes = 0;)

    bool window_has_area() const {
        RECT rect{};
        const auto window = swap_chain->getWindow();
        return GetClientRect(window, &rect) && !IsIconic(window) &&
               rect.right > rect.left && rect.bottom > rect.top;
    }

    void release_pin(const PlumePresenter::NativePin& pin, bool deferred) {
        if (!pin.valid || native_release_hook == nullptr) return;
        native_release_hook(native_release_context, pin.slot, pin.generation);
        if (deferred) {
            ++stats.pins_released_deferred;
        } else {
            ++stats.pins_released_immediate;
        }
    }

    void release_slot_pins(Slot& frame) {
        GEKKO_PROFILE_ONLY(if (frame.pin_count != 0u) {
            GEKKO_PROFILE_PLOT(
                "host/present pin hold ms",
                static_cast<double>(elapsed_ns_since(frame.pins_adopted_at)) / 1.0e6);
        })
        for (u32 index = 0; index < frame.pin_count; ++index) {
            release_pin(frame.pins[index], true);
            frame.pins[index] = PlumePresenter::NativePin{};
        }
        frame.pin_count = 0;
    }

    // Non-blocking counterpart of wait_slot's completion test. Completion is
    // judged solely by GetCompletedValue, never by the auto-reset event, which
    // can carry a stale signal from a previous use of this slot.
    void fence_failed(Slot& frame, plume::RenderFenceWait outcome) {
        frame.retirement_outcome = outcome;
        failed = true;
        ready = false;
        terminal_failure.publish(outcome, [&](plume::RenderFenceWait first) {
            const plume::FenceRetirementError error(first, "native presentation slot");
            std::fprintf(stderr, "[dx12-present-fence] terminal failure: %s\n", error.what());
            std::fflush(stderr);
            terminal_error = error.what();
        });
    }

    bool slot_fence_retired(Slot& frame) {
        auto* native_fence = static_cast<plume::D3D12CommandFence*>(frame.fence.get());
        if (native_fence == nullptr || native_fence->d3d == nullptr ||
            native_fence->fenceValue == 0u) {
            fence_failed(frame, plume::RenderFenceWait::WAIT_ERROR);
            return false;
        }
        const auto outcome = plume::classifyD3D12Fence(native_fence->d3d->GetCompletedValue(),
                                                      native_fence->fenceValue - 1u, false);
        if (outcome == plume::RenderFenceWait::DEVICE_LOST) fence_failed(frame, outcome);
        return outcome == plume::RenderFenceWait::SIGNALED;
    }

    // The shared presentation queue mutex, with contention accounting. The
    // Flipper holds it only around its own executeCommandLists
    // (plume_gx_raster.cpp:3205-3210), so a miss here is a real collision
    // between scan-out and raster submission, not a lock held across work.
    std::unique_lock<std::mutex> lock_queue() {
        std::unique_lock<std::mutex> lock;
        if (queue_mutex == nullptr) {
            return lock;
        }
        if (!queue_mutex->try_lock()) {
            ++stats.queue_lock_misses;
            const auto start = std::chrono::steady_clock::now();
            queue_mutex->lock();
            GEKKO_PROFILE_ONLY(const u64 wait_before = stats.queue_lock_wait_ns;)
            stats.queue_lock_wait_ns += elapsed_ns_since(start);
            // Emitted only on a miss, so the plot is empty in an uncontended
            // run. queue_lock_misses counts collisions and queue_lock_wait_ns
            // totals them, but neither says WHEN, so a collision could not be
            // lined up against the Flipper's own submission.
            GEKKO_PROFILE_PLOT(
                "host/present queue lock wait ms",
                static_cast<double>(stats.queue_lock_wait_ns - wait_before) / 1.0e6);
        }
        lock = std::unique_lock<std::mutex>(*queue_mutex, std::adopt_lock);
        return lock;
    }

    void record_present(std::chrono::steady_clock::time_point start) {
        const u64 ns = elapsed_ns_since(start);
        ++stats.presents;
        stats.present_ns += ns;
        if (ns > stats.present_ns_max) stats.present_ns_max = ns;
        // All three present paths funnel through here, so one site covers
        // native, CPU-blit and CPU-staging with no new computation and no new
        // timing call. present_ns_max is a lifetime maximum; this is the
        // distribution, and shadow_main's ">33.4 ms [present-gap]" log line
        // becomes a visible spike instead.
        GEKKO_PROFILE_PLOT("host/present latency ms", static_cast<double>(ns) / 1.0e6);
        // Occupancy of the three flight slots, and the presenter's share of the
        // rasterizer's NativeXFB ring. Pinned at 3 means the presenter is
        // GPU-bound and host/present-wait-slot will be blocking; sitting at 1
        // means it is not. Caveat: on --present-dump and --verify-plume runs
        // wait_slot is called immediately after in_flight is set, so both
        // readings are one lower there.
        GEKKO_PROFILE_ONLY(
            u32 slots_in_flight = 0;
            u32 pins_held = 0;
            for (int index = 0; index < kFlight; ++index) {
                if (slots[index].in_flight) ++slots_in_flight;
                pins_held += slots[index].pin_count;
            }
            GEKKO_PROFILE_PLOT("host/present frames in flight",
                               static_cast<double>(slots_in_flight));
            GEKKO_PROFILE_PLOT("host/present pins held", static_cast<double>(pins_held));)
    }

    bool create_blit_resources() {
        const presentshaders::EmbeddedShader* vertex_dxbc = find_present_shader("VSPresentBlit");
        const presentshaders::EmbeddedShader* pixel_dxbc = find_present_shader("PSPresentBlit");
        const presentshaders::EmbeddedShader* blend_dxbc = find_present_shader("PSPresentBlend");
        if (vertex_dxbc == nullptr || pixel_dxbc == nullptr || blend_dxbc == nullptr) return false;
        blit_vertex_shader = device->createShader(vertex_dxbc->dxbc, vertex_dxbc->size,
                                                  "VSPresentBlit",
                                                  plume::RenderShaderFormat::DXIL);
        blit_pixel_shader = device->createShader(pixel_dxbc->dxbc, pixel_dxbc->size,
                                                 "PSPresentBlit",
                                                 plume::RenderShaderFormat::DXIL);
        blend_pixel_shader = device->createShader(blend_dxbc->dxbc, blend_dxbc->size,
                                                  "PSPresentBlend",
                                                  plume::RenderShaderFormat::DXIL);
        if (blit_vertex_shader == nullptr || blit_pixel_shader == nullptr ||
            blend_pixel_shader == nullptr) return false;

        plume::RenderSamplerDesc sampler_desc;
        sampler_desc.minFilter = plume::RenderFilter::LINEAR;
        sampler_desc.magFilter = plume::RenderFilter::LINEAR;
        sampler_desc.mipmapMode = plume::RenderMipmapMode::NEAREST;
        sampler_desc.addressU = plume::RenderTextureAddressMode::CLAMP;
        sampler_desc.addressV = plume::RenderTextureAddressMode::CLAMP;
        sampler_desc.addressW = plume::RenderTextureAddressMode::CLAMP;
        blit_sampler = device->createSampler(sampler_desc);
        if (blit_sampler == nullptr) return false;

        // One set holding the frame texture; the sampler is immutable in the
        // layout so it cannot drift per frame. NOTE: it is NOT free - plume counts an
        // immutable sampler as a VIEW descriptor (plume_d3d12.cpp:905 treats a range as a
        // sampler-heap range only when immutableSampler == nullptr), so this set is THREE
        // views per slot. kPresenterDescriptors in plume_gx_raster.cpp must reserve that;
        // reserving two crashed every internal scale above 1.
        const plume::RenderSampler* immutable_sampler = blit_sampler.get();
        const plume::RenderDescriptorRange ranges[] = {
            plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 2),
            plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 0, 1,
                                         &immutable_sampler),
        };
        const plume::RenderDescriptorSetDesc set_desc(ranges,
                                                      static_cast<u32>(std::size(ranges)));
        const plume::RenderPushConstantRange blend_constant(
            0, 0, 0, sizeof(float) * 2u, plume::RenderShaderStageFlag::PIXEL);
        plume::RenderPipelineLayoutDesc layout_desc;
        layout_desc.pushConstantRanges = &blend_constant;
        layout_desc.pushConstantRangesCount = 1;
        layout_desc.descriptorSetDescs = &set_desc;
        layout_desc.descriptorSetDescsCount = 1;
        layout_desc.allowInputLayout = false;
        blit_layout = device->createPipelineLayout(layout_desc);
        if (blit_layout == nullptr) return false;

        plume::RenderGraphicsPipelineDesc pipeline_desc;
        pipeline_desc.pipelineLayout = blit_layout.get();
        pipeline_desc.vertexShader = blit_vertex_shader.get();
        pipeline_desc.pixelShader = blit_pixel_shader.get();
        pipeline_desc.renderTargetFormat[0] = plume::RenderFormat::B8G8R8A8_UNORM;
        pipeline_desc.renderTargetCount = 1;
        pipeline_desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
        pipeline_desc.cullMode = plume::RenderCullMode::NONE;
        pipeline_desc.depthEnabled = false;
        pipeline_desc.depthWriteEnabled = false;
        blit_pipeline = device->createGraphicsPipeline(pipeline_desc);
        pipeline_desc.pixelShader = blend_pixel_shader.get();
        blend_pipeline = device->createGraphicsPipeline(pipeline_desc);
        if (blit_pipeline == nullptr || blend_pipeline == nullptr) return false;

        for (int index = 0; index < kFlight; ++index) {
            slots[index].source_descriptor = device->createDescriptorSet(set_desc);
            if (slots[index].source_descriptor == nullptr) return false;
        }
        return true;
    }

    std::unique_ptr<plume::RenderShader> blit_vertex_shader;
    std::unique_ptr<plume::RenderShader> blit_pixel_shader;

    void wait_slot(Slot& frame) {
        if (!frame.in_flight) return;
        // Opened BELOW the guard on purpose: wait_slot is also called in
        // three-slot loops from the resize branches and from destroy, and most
        // of those calls return here immediately. slot_waits /
        // slot_waits_blocked / slot_wait_ns_max are lifetime aggregates, so one
        // 10 s fence deadline and a steady 4 ms per-frame block read the same
        // in the exit report.
        GEKKO_PROFILE_ZONE("host/present-wait-slot");
        auto* native_fence =
            static_cast<plume::D3D12CommandFence*>(frame.fence.get());
        auto* native_device = static_cast<plume::D3D12Device*>(device);
        if (native_fence == nullptr || native_fence->d3d == nullptr ||
            native_device == nullptr || native_device->d3d == nullptr ||
            native_fence->fenceValue == 0u) {
            fence_failed(frame, plume::RenderFenceWait::WAIT_ERROR);
            return;
        }
        const UINT64 target = native_fence->fenceValue - 1u;
        GEKKO_PROFILE_VALUE(target);
        ++stats.slot_waits;
        const auto wait_start = std::chrono::steady_clock::now();
        const auto account_wait = [&]() {
            const u64 ns = elapsed_ns_since(wait_start);
            stats.slot_wait_ns += ns;
            if (ns > stats.slot_wait_ns_max) stats.slot_wait_ns_max = ns;
        };
        const auto initial = plume::classifyD3D12Fence(native_fence->d3d->GetCompletedValue(), target, false);
        if (initial == plume::RenderFenceWait::SIGNALED) {
            render::observePresentationRetirement(frame.in_flight, initial, [&] { release_slot_pins(frame); });
            account_wait();
            return;
        }
        if (initial == plume::RenderFenceWait::TIMEOUT) ++stats.slot_waits_blocked;
        const auto result = plume::waitForRetirement([&](u64 timeout) {
            return timeout == 0u ? initial : queue->waitForCommandFenceFor(frame.fence.get(), timeout);
        });
        frame.retirement_outcome = result.outcome;
        if (!render::observePresentationRetirement(frame.in_flight, result.outcome,
                                                  [&] { release_slot_pins(frame); })) {
            // Keep resources and source leases GPU-owned; the process owner
            // stops presentation/game publication instead of hiding it in GDI.
            fence_failed(frame, result.outcome);
        }
        account_wait();
    }

};

PlumePresenter::PlumePresenter() : impl_(std::make_unique<Impl>()) {
    // Declared in the CONSTRUCTOR, not in create(): there are two create
    // overloads - the owned-device CPU-upload path and the external-device
    // native path - and configuring the plots in only one leaves them
    // unconfigured on every run that takes the other. The constructor runs on
    // both, and a config for a plot that never receives a point is harmless.
    // Counts are staircases; millisecond curves interpolate.
    GEKKO_PROFILE_PLOT_CONFIG("host/present latency ms", GEKKO_PLOT_NUMBER, false, true, 0x60FFA0);
    GEKKO_PROFILE_PLOT_CONFIG("host/present queue lock wait ms", GEKKO_PLOT_NUMBER, false, true,
                              0xFF6060);
    GEKKO_PROFILE_PLOT_CONFIG("host/present pin hold ms", GEKKO_PLOT_NUMBER, false, true,
                              0xFFC060);
    GEKKO_PROFILE_PLOT_CONFIG("host/present frames in flight", GEKKO_PLOT_NUMBER, true, true,
                              0x60C0FF);
    GEKKO_PROFILE_PLOT_CONFIG("host/present pins held", GEKKO_PLOT_NUMBER, true, true, 0xC060FF);
    GEKKO_PROFILE_PLOT_CONFIG("host/present refusals", GEKKO_PLOT_NUMBER, true, true, 0xFF4040);
    GEKKO_PROFILE_PLOT_CONFIG("host/present staging bytes", GEKKO_PLOT_MEMORY, true, true,
                              0xA0A0FF);
}
PlumePresenter::~PlumePresenter() { destroy(); }

bool PlumePresenter::available() const noexcept { return impl_ && impl_->ready; }
bool PlumePresenter::failed() const noexcept { return !impl_ || impl_->failed; }
bool PlumePresenter::fence_failed() const noexcept {
    return impl_ && impl_->terminal_failure.failed();
}
std::string PlumePresenter::last_error() const {
    return impl_ ? impl_->terminal_error : std::string{};
}

bool PlumePresenter::create(void* native_window) {
    if (native_window == nullptr) return false;
    Impl& impl = *impl_;
    if (impl.terminal_failure.failed()) return false;
    auto d3d12 = std::make_unique<plume::D3D12Interface>();
    if (!d3d12->isValid()) {
        impl.failed = true;
        return false;
    }
    impl.interface_ = std::move(d3d12);
    impl.owned_device = impl.interface_->createDevice();
    if (impl.owned_device == nullptr) {
        impl.failed = true;
        return false;
    }
    impl.owned_queue =
        impl.owned_device->createCommandQueue(plume::RenderCommandListType::DIRECT);
    if (impl.owned_queue == nullptr) {
        impl.failed = true;
        return false;
    }
    return create(native_window, impl.owned_device.get(), impl.owned_queue.get(), nullptr);
}

bool PlumePresenter::create(void* native_window, void* external_device, void* external_queue,
                            void* external_queue_mutex) {
    if (native_window == nullptr || external_device == nullptr || external_queue == nullptr) {
        return false;
    }
    Impl& impl = *impl_;
    if (impl.terminal_failure.failed()) return false;
    impl.device = static_cast<plume::RenderDevice*>(external_device);
    impl.queue = static_cast<plume::RenderCommandQueue*>(external_queue);
    impl.queue_mutex = static_cast<std::mutex*>(external_queue_mutex);

    // Triple-buffered swap chain, B8G8R8A8 to match the byte order the runtime
    // already produces: one u32 per pixel as 0xAARRGGBB, which is B,G,R,A in
    // memory - the same layout the GDI path hands to BITMAPINFO with BI_RGB.
    const plume::RenderSwapChainDesc desc(static_cast<plume::RenderWindow>(native_window),
                                          plume::RenderFormat::B8G8R8A8_UNORM, Impl::kFlight,
                                          false, 1);
    impl.swap_chain = impl.queue->createSwapChain(desc);
    if (impl.swap_chain == nullptr) {
        impl.failed = true;
        return false;
    }
    // Wait on display refresh only here: Present runs on the UI thread and
    // only for a completed XFB generation (empty retraces are never uploaded).
    // That lets the GPU idle between PAL fields. Guest time stays on the
    // recovered 486 MHz / VI clock; DXGI Present is not the game clock.
    impl.swap_chain->setVsyncEnabled(impl.vsync);

    // Optional: if this fails the presenter keeps working, just on the CPU
    // resampler. It is not worth refusing to present over.
    impl.blit_ready = impl.blit_requested && impl.create_blit_resources();
    if (impl.blit_requested && !impl.blit_ready) {
        std::fprintf(stderr,
                     "[present] GPU blit unavailable on this adapter - falling back to the "
                     "CPU resampler\n");
    }

    for (int index = 0; index < Impl::kFlight; ++index) {
        impl.slots[index].commands = impl.queue->createCommandList();
        impl.slots[index].fence = impl.device->createCommandFence();
        if (impl.slots[index].commands == nullptr || impl.slots[index].fence == nullptr) {
            impl.failed = true;
            return false;
        }
    }

    impl.ready = true;
    return true;
}

void PlumePresenter::destroy() {
    if (!impl_) return;
    Impl& impl = *impl_;
    if (impl.queue != nullptr) {
        for (int index = 0; index < Impl::kFlight; ++index)
            impl.wait_slot(impl.slots[index]);
    }
    // Preflight the whole dependency graph BEFORE destroying any member or
    // returning a source lease. Timeout/wait error leave GPU ownership intact.
    // Confirmed device removal stops further GPU consumption and permits disposal.
    for (int index = 0; index < Impl::kFlight; ++index) {
        const Impl::Slot& slot = impl.slots[index];
        if (!render::canDisposePresentation(slot.in_flight, slot.retirement_outcome)) {
            std::fprintf(stderr,
                         "[dx12-present-fence] cannot safely destroy unretired slot %d; "
                         "terminating before releasing GPU resources or source leases\n", index);
            std::fflush(stderr);
            std::terminate();
        }
    }
    for (int index = 0; index < Impl::kFlight; ++index) {
        Impl::Slot& slot = impl.slots[index];
        if (slot.in_flight) {
            render::disposeRemovedPresentation(slot.in_flight, slot.retirement_outcome,
                                               [&] { impl.release_slot_pins(slot); });
        }
        if (slot.upload != nullptr && slot.mapped != nullptr) {
            slot.upload->unmap();
            slot.mapped = nullptr;
        }
        GEKKO_PROFILE_ONLY(if (slot.upload != nullptr) {
            GEKKO_PROFILE_FREE(slot.upload.get(), "present-staging");
            impl.staging_bytes -= slot.upload_size;
        })
        slot.upload.reset();
        slot.commands.reset();
        slot.fence.reset();
        slot.source_descriptor.reset();
        GEKKO_PROFILE_ONLY(if (slot.source != nullptr) {
            GEKKO_PROFILE_FREE(slot.source.get(), "present-staging");
            impl.staging_bytes -=
                static_cast<u64>(slot.source_width) * slot.source_height * sizeof(u32);
        })
        slot.source.reset();
        slot.source_width = 0;
        slot.source_height = 0;
        slot.upload_size = 0;
        slot.in_flight = false;
        slot.cleared = false;
        impl.release_slot_pins(slot);
    }
    // Framebuffers reference swap-chain textures; dispose them before the
    // swap chain, and the pipeline before its layout, shaders and sampler.
    for (int index = 0; index < Impl::kFlight; ++index) {
        impl.blit_framebuffers[index].reset();
        impl.blit_framebuffer_target[index] = nullptr;
    }
    impl.blend_pipeline.reset();
    impl.blit_pipeline.reset();
    impl.blit_layout.reset();
    impl.blit_vertex_shader.reset();
    impl.blend_pixel_shader.reset();
    impl.blit_pixel_shader.reset();
    impl.blit_sampler.reset();
    impl.blit_ready = false;
    GEKKO_PROFILE_ONLY(if (impl.readback != nullptr) {
        GEKKO_PROFILE_FREE(impl.readback.get(), "present-staging");
        impl.staging_bytes -= impl.readback_size;
    })
    impl.readback.reset();
    impl.readback_size = 0;
    impl.swap_chain.reset();
    GEKKO_PROFILE_PLOT("host/present staging bytes", static_cast<double>(impl.staging_bytes));
    impl.queue = nullptr;
    impl.device = nullptr;
    impl.queue_mutex = nullptr;
    impl.owned_queue.reset();
    impl.owned_device.reset();
    impl.interface_.reset();
    impl.ready = false;
}

void PlumePresenter::set_present_dump(const char* path, int frame) {
    if (impl_ != nullptr && path != nullptr) {
        impl_->blit_dump_path = path;
        impl_->blit_dump_countdown = frame > 0 ? frame : 30;
    }
}

void PlumePresenter::set_gpu_blit(bool enabled) noexcept {
    if (impl_ != nullptr) {
        impl_->blit_requested = enabled;
    }
}

void PlumePresenter::set_area_resolve(bool enabled) noexcept {
    if (impl_ != nullptr) {
        impl_->area_resolve = enabled;
    }
}

void PlumePresenter::set_vsync(bool enabled) noexcept {
    if (impl_ != nullptr) {
        impl_->vsync = enabled;
    }
}

void PlumePresenter::set_verify(bool enabled) noexcept {
    if (impl_) {
        impl_->verify = enabled;
    }
}

PlumePresenter::VerifyReport PlumePresenter::verify_report() const {
    return impl_ ? impl_->report : VerifyReport{};
}

void PlumePresenter::set_narrow_queue_lock(bool enabled) noexcept {
    if (impl_ != nullptr) {
        impl_->narrow_queue_lock = enabled;
    }
}

void PlumePresenter::set_native_release_hook(NativeReleaseHook hook, void* context) noexcept {
    if (impl_ != nullptr) {
        impl_->native_release_hook = hook;
        impl_->native_release_context = context;
    }
}

void PlumePresenter::set_deferred_pin_release(bool enabled) noexcept {
    if (impl_ != nullptr) {
        impl_->deferred_pin_release = enabled;
    }
}

bool PlumePresenter::deferred_pin_release_active() const noexcept {
    return impl_ != nullptr && impl_->ready && impl_->deferred_pin_release &&
           impl_->native_release_hook != nullptr;
}

void PlumePresenter::retire_completed_presents() {
    if (!impl_) {
        return;
    }
    Impl& impl = *impl_;
    if (impl.native_release_hook == nullptr) {
        return;
    }
    for (int index = 0; index < Impl::kFlight; ++index) {
        Impl::Slot& frame = impl.slots[index];
        // Only slots that still owe a pin are worth polling: a slot holding
        // nothing gains nothing from being marked retired early, and leaving
        // in_flight alone keeps the present path's behaviour identical.
        if (frame.pin_count == 0u || !frame.in_flight) {
            continue;
        }
        if (!impl.slot_fence_retired(frame)) {
            continue;
        }
        render::observePresentationRetirement(frame.in_flight, plume::RenderFenceWait::SIGNALED,
                                              [&] { impl.release_slot_pins(frame); });
        ++impl.stats.slots_retired_early;
    }
}

PlumePresenter::PresentStats PlumePresenter::present_stats() const {
    return impl_ ? impl_->stats : PresentStats{};
}

bool PlumePresenter::present_native(void* texture, void* previous_texture,
                                    float blend_alpha, int width, int height,
                                    float content_aspect, const NativePin* source_pin,
                                    const NativePin* previous_pin) {
    GEKKO_PROFILE_ZONE("host/present-native");
    // D06-F7. Latched once, from the same predicate the caller queried a
    // moment ago on this same thread, so ownership of each pin is unambiguous
    // for the whole call: either this function hands it back (immediately if
    // the present never reaches the queue, on the flight fence if it does) or
    // the caller does, never both.
    const bool adopt = deferred_pin_release_active();
    NativePin adopted[2];
    u32 adopted_count = 0;
    if (adopt) {
        if (source_pin != nullptr && source_pin->valid) adopted[adopted_count++] = *source_pin;
        if (previous_pin != nullptr && previous_pin->valid) adopted[adopted_count++] = *previous_pin;
        impl_->stats.pins_adopted += adopted_count;
    }
    const auto hand_back_now = [&]() {
        for (u32 index = 0; index < adopted_count; ++index) {
            impl_->release_pin(adopted[index], false);
        }
        adopted_count = 0;
    };
    if (!impl_ || !impl_->ready || texture == nullptr || width <= 0 || height <= 0 ||
        !impl_->blit_ready || impl_->verify || impl_->owned_device != nullptr) {
        hand_back_now();
        // Every false return of this function bumps one monotone plot: the
        // slope is the refusal rate and the timeline says when, which is what
        // the lifetime presents_skipped_minimised / present_failures counters
        // cannot do. The three exits that mean a genuine FAULT rather than a
        // hidden window also emit a message, because a plot carries one number
        // and cannot say which of eleven exits fired.
        GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
        return false;
    }
    Impl& impl = *impl_;
    if (!impl.window_has_area()) {
        ++impl.stats.presents_skipped_minimised;
        hand_back_now();
        GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
        return false;
    }
    if (impl.swap_chain->needsResize()) {
        // A multi-frame stall that otherwise appears as an unexplained gap in
        // the present cadence: it blocks on all three flight fences, resizes
        // the swap chain and drops the framebuffer cache. It repeats for every
        // mouse move during a window drag - the same drag that parks
        // host/window-pump in the Win32 modal loop, so the two zones together
        // account for the whole stall.
        GEKKO_PROFILE_ZONE("host/present-resize");
        for (int index = 0; index < Impl::kFlight; ++index) impl.wait_slot(impl.slots[index]);
        if (!impl.ready) {
            hand_back_now();
            GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
            return false;
        }
        if (!impl.swap_chain->resize()) {
            impl.failed = true;
            impl.ready = false;
            hand_back_now();
            GEKKO_PROFILE_MESSAGE_LITERAL("host/present: swap chain resize failed");
            GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
            return false;
        }
        for (int index = 0; index < Impl::kFlight; ++index) {
            impl.blit_framebuffers[index].reset();
            impl.blit_framebuffer_target[index] = nullptr;
        }
    }
    if (impl.swap_chain->isEmpty()) {
        // Minimised, or a client area of 0x0: there is nothing to present and
        // this is not a failure. Counted so a run of "no frames on screen" can
        // be told apart from a dead presenter (D06-F5).
        ++impl.stats.presents_skipped_minimised;
        hand_back_now();
        GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
        return false;
    }
    const u32 surface_width = impl.swap_chain->getWidth();
    const u32 surface_height = impl.swap_chain->getHeight();
    if (surface_width == 0u || surface_height == 0u) {
        ++impl.stats.presents_skipped_minimised;
        hand_back_now();
        GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
        return false;
    }
    Impl::Slot& slot = impl.slots[impl.slot];
    impl.wait_slot(slot);
    if (!impl.ready) {
        hand_back_now();
        GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
        return false;
    }
    u32 texture_index = 0;
    {
        // Braced only so the zone ends at the acquire. This is the phase that
        // blocks against DXGI's frame-latency limit, and it is untimed on both
        // paths: record_present starts its clock at the Present() call, so
        // anything lost before that is attributed to nothing.
        GEKKO_PROFILE_ZONE("host/present-acquire");
        if (!impl.swap_chain->acquireTexture(nullptr, &texture_index)) {
            hand_back_now();
            GEKKO_PROFILE_MESSAGE_LITERAL("host/present: acquireTexture failed");
            GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
            return false;
        }
    }
    plume::RenderTexture* back_buffer = impl.swap_chain->getTexture(texture_index);
    if (back_buffer == nullptr) {
        hand_back_now();
        GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
        return false;
    }
    const float aspect = content_aspect > 0.1f
                             ? content_aspect
                             : static_cast<float>(width) / static_cast<float>(height);
    const ContentRect dest = fit_content_rect(static_cast<int>(surface_width),
                                              static_cast<int>(surface_height), aspect);
    if (impl.blit_framebuffer_target[texture_index] != back_buffer ||
        impl.blit_framebuffers[texture_index] == nullptr) {
        const plume::RenderTexture* attachment = back_buffer;
        impl.blit_framebuffers[texture_index] =
            impl.device->createFramebuffer(plume::RenderFramebufferDesc(&attachment, 1));
        impl.blit_framebuffer_target[texture_index] = back_buffer;
        if (impl.blit_framebuffers[texture_index] == nullptr) {
            hand_back_now();
            GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
            return false;
        }
    }
    auto* source = static_cast<plume::RenderTexture*>(texture);
    auto* previous = previous_texture != nullptr
                         ? static_cast<plume::RenderTexture*>(previous_texture)
                         : source;
    const bool blending = previous != source && blend_alpha < 1.0f;
    const float alpha = std::clamp(blend_alpha, 0.0f, 1.0f);
    const bool dumping = !impl.blit_dump_path.empty() && impl.blit_dump_countdown > 0 &&
                         --impl.blit_dump_countdown == 0;
    const u32 dump_row_pixels = aligned_row_pixels(surface_width);
    if (dumping) {
        const u64 dump_bytes =
            static_cast<u64>(dump_row_pixels) * surface_height * sizeof(u32);
        if (impl.readback == nullptr || impl.readback_size < dump_bytes) {
            // The FREE has to precede the assignment: unique_ptr::operator=
            // destroys the old buffer, and a pool whose free arrives after the
            // replacement reads as a double free in Tracy.
            GEKKO_PROFILE_ONLY(if (impl.readback != nullptr) {
                GEKKO_PROFILE_FREE(impl.readback.get(), "present-staging");
                impl.staging_bytes -= impl.readback_size;
            })
            impl.readback =
                impl.device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(dump_bytes));
            impl.readback_size = impl.readback != nullptr ? dump_bytes : 0;
            GEKKO_PROFILE_ONLY(if (impl.readback != nullptr) {
                impl.staging_bytes += impl.readback_size;
                GEKKO_PROFILE_ALLOC(impl.readback.get(), impl.readback_size, "present-staging");
            })
            GEKKO_PROFILE_PLOT("host/present staging bytes", static_cast<double>(impl.staging_bytes));
        }
    }
    {
        // D06-F6: everything recorded below touches only objects this
        // presenter owns - this flight slot's descriptor set (its own range of
        // the device view heap, written with CreateShaderResourceView, which
        // is free-threaded) and its own command list. The only shared object
        // is the queue, so by default the mutex is taken around
        // executeCommandLists alone, exactly as the CPU-upload path has always
        // done (:894-901). set_narrow_queue_lock(false) restores the wide
        // scope for bisection. Same commands, same order, same submission -
        // the guest cannot observe either shape.
        //
        // The zone opens at this brace rather than at begin() below: with
        // --no-narrow-present-lock the WIDE queue lock is taken right here, and
        // a zone starting at begin() would leave that wait attributed to
        // nothing on the one configuration where it matters.
        GEKKO_PROFILE_ZONE("host/present-record");
        std::unique_lock<std::mutex> wide_queue_lock;
        if (!impl.narrow_queue_lock) {
            wide_queue_lock = impl.lock_queue();
        }
        slot.source_descriptor->setTexture(0, source,
                                           plume::RenderTextureLayout::SHADER_READ);
        slot.source_descriptor->setTexture(1, previous,
                                           plume::RenderTextureLayout::SHADER_READ);
        slot.commands->begin();
        // The producing submission already transitioned the NativeXFB to
        // SHADER_READ, but that barrier lives on the Flipper command list.
        // Repeat presents (and a predecessor used for the cross-fade) must
        // name the same state on THIS list so a UAV rewrite of a recycled
        // slot cannot race the blit on the shared DIRECT queue.
        const plume::RenderTextureBarrier present_barriers[] = {
            plume::RenderTextureBarrier(back_buffer, plume::RenderTextureLayout::COLOR_WRITE),
            plume::RenderTextureBarrier(source, plume::RenderTextureLayout::SHADER_READ),
            plume::RenderTextureBarrier(previous, plume::RenderTextureLayout::SHADER_READ),
        };
        const u32 present_barrier_count = previous != source ? 3u : 2u;
        slot.commands->barriers(plume::RenderBarrierStage::GRAPHICS, present_barriers,
                                present_barrier_count);
        slot.commands->setFramebuffer(impl.blit_framebuffers[texture_index].get());
        // Only the letterbox/pillarbox bars need a clear. When the content rect
        // is the whole surface the opaque blit below writes every pixel, so a
        // full-surface clear on every present (8.3M px at 4K) was pure overdraw
        // - the common case, since aspect=auto fits the window at >= 4:3.
        const bool content_covers_surface =
            dest.x == 0 && dest.y == 0 && dest.width == static_cast<int>(surface_width) &&
            dest.height == static_cast<int>(surface_height);
        if (!content_covers_surface) {
            slot.commands->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
        }
        slot.commands->setGraphicsPipelineLayout(impl.blit_layout.get());
        slot.commands->setPipeline(blending ? impl.blend_pipeline.get() : impl.blit_pipeline.get());
        slot.commands->setGraphicsDescriptorSet(slot.source_descriptor.get(), 0);
        struct {
            float alpha;
            std::int32_t max_taps;
        } present_constants{alpha, impl.area_resolve ? 8 : 1};
        slot.commands->setGraphicsPushConstants(0, &present_constants);
        const plume::RenderViewport viewport(
            static_cast<float>(dest.x), static_cast<float>(dest.y),
            static_cast<float>(dest.width), static_cast<float>(dest.height));
        slot.commands->setViewports(&viewport, 1);
        const plume::RenderRect scissor(dest.x, dest.y, dest.x + dest.width,
                                        dest.y + dest.height);
        slot.commands->setScissors(&scissor, 1);
        slot.commands->drawInstanced(3, 1, 0, 0);
        if (dumping && impl.readback != nullptr) {
            slot.commands->barriers(
                plume::RenderBarrierStage::COPY,
                plume::RenderTextureBarrier(back_buffer,
                                            plume::RenderTextureLayout::COPY_SOURCE));
            auto destination = plume::RenderTextureCopyLocation::PlacedFootprint(
                impl.readback.get(), plume::RenderFormat::B8G8R8A8_UNORM, surface_width,
                surface_height, 1, dump_row_pixels);
            destination.texture = back_buffer;
            slot.commands->copyTextureRegion(
                destination, plume::RenderTextureCopyLocation::Subresource(back_buffer));
        }
        slot.commands->barriers(
            plume::RenderBarrierStage::NONE,
            plume::RenderTextureBarrier(back_buffer, plume::RenderTextureLayout::PRESENT));
        slot.commands->end();
        const plume::RenderCommandList* list = slot.commands.get();
        plume::RenderCommandSemaphore* xfb_waits[2] = {};
        u32 xfb_wait_count = 0;
        const auto add_xfb_wait = [&](const NativePin* pin) {
            if (pin == nullptr || !pin->valid || pin->ready_semaphore == nullptr) return;
            auto* sem = static_cast<plume::RenderCommandSemaphore*>(pin->ready_semaphore);
            for (u32 index = 0; index < xfb_wait_count; ++index) {
                if (xfb_waits[index] == sem) return;
            }
            if (xfb_wait_count < 2u) xfb_waits[xfb_wait_count++] = sem;
        };
        add_xfb_wait(source_pin);
        add_xfb_wait(previous_pin);
        {
            // The only point where the presenter and the Flipper meet: one
            // shared DIRECT queue behind queue_mutex. The zone deliberately
            // INCLUDES lock_queue, because the wait is the interesting part -
            // queue_lock_misses says how often, never when.
            GEKKO_PROFILE_ZONE("host/present-submit");
            std::unique_lock<std::mutex> execute_lock;
            if (impl.narrow_queue_lock) {
                execute_lock = impl.lock_queue();
            }
            impl.queue->executeCommandLists(&list, 1,
                                            xfb_wait_count != 0u ? xfb_waits : nullptr,
                                            xfb_wait_count, nullptr, 0, slot.fence.get());
        }
        if (xfb_wait_count != 0u) ++impl.stats.xfb_ready_waits;
    }
    slot.in_flight = true;
    slot.retirement_outcome = plume::RenderFenceWait::TIMEOUT;
    // D06-F7: the blit is on the queue, so the pins ride on this flight slot
    // and go back when its fence retires. Any straggler here belongs to a slot
    // whose fence has already been observed retired, so it is a deferred
    // release too.
    if (adopted_count != 0u) {
        impl.release_slot_pins(slot);
        for (u32 index = 0; index < adopted_count; ++index) {
            slot.pins[index] = adopted[index];
        }
        slot.pin_count = adopted_count;
        GEKKO_PROFILE_ONLY(slot.pins_adopted_at = std::chrono::steady_clock::now();)
        adopted_count = 0;
    }
    if (dumping && impl.readback != nullptr) {
        // Blocking wait_slot, a full-surface map and a PPM write, all on the
        // presenting thread. This is the single unexplained multi-hundred-ms
        // hitch in a --present-dump capture.
        GEKKO_PROFILE_ZONE("host/present-readback");
        impl.wait_slot(slot);
        if (!impl.ready) return false;
        if (const u32* seen = static_cast<const u32*>(impl.readback->map())) {
            if (std::FILE* file = std::fopen(impl.blit_dump_path.c_str(), "wb")) {
                std::fprintf(file, "P6\n%u %u\n255\n", surface_width, surface_height);
                std::vector<unsigned char> row(static_cast<std::size_t>(surface_width) * 3u);
                for (u32 y = 0; y < surface_height; ++y) {
                    const u32* line = seen + static_cast<std::size_t>(y) * dump_row_pixels;
                    for (u32 x = 0; x < surface_width; ++x) {
                        row[x * 3u + 0u] = static_cast<unsigned char>((line[x] >> 16) & 0xffu);
                        row[x * 3u + 1u] = static_cast<unsigned char>((line[x] >> 8) & 0xffu);
                        row[x * 3u + 2u] = static_cast<unsigned char>(line[x] & 0xffu);
                    }
                    std::fwrite(row.data(), 1, row.size(), file);
                }
                std::fclose(file);
            }
            impl.readback->unmap();
        }
        impl.blit_dump_path.clear();
    }
    // D06-F5: Present blocks when the flip queue is full (max frame latency 1),
    // so its duration is the presenter's share of the display cadence and the
    // only place the swap chain's backpressure is visible from the CPU.
    {
        // The zone spans exactly the interval record_present already measures;
        // what it adds is POSITION on the timeline next to acquire, record and
        // submit, which is what makes a present gap attributable. The brace has
        // to cover the failure branch as well: `presented` is declared here and
        // read by it.
        GEKKO_PROFILE_ZONE("host/present-swap");
        const auto present_start = std::chrono::steady_clock::now();
        const bool presented = impl.swap_chain->present(texture_index, nullptr, 0);
        impl.record_present(present_start);
        if (!presented) {
            ++impl.stats.present_failures;
            ++impl.stats.present_failures_consecutive;
            GEKKO_PROFILE_MESSAGE_LITERAL("host/present: Present() failed");
            GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
            std::fprintf(stderr,
                         "[dx12-present] Present() failed (consecutive %llu)\n",
                         static_cast<unsigned long long>(impl.stats.present_failures_consecutive));
            std::fflush(stderr);
            // One DXGI failure (alt-tab, occluded, a single lost frame) used to
            // mark the presenter dead. The UI then fell through to an empty CPU
            // frame (native path never publishes pixels) and the last back buffer
            // - often the clearColor black issued just above - stayed on screen
            // forever. Three consecutive failures still take the presenter down;
            // a single miss just skips this scan-out. Pins ride on the in-flight
            // slot and come back from wait_slot / destroy.
            if (impl.stats.present_failures_consecutive >= 3u) {
                impl.failed = true;
                impl.ready = false;
                impl.release_slot_pins(slot);
            }
            return false;
        }
    }
    impl.stats.present_failures_consecutive = 0;
    ++impl.report.native_xfb_frames;
    impl.slot = (impl.slot + 1) % Impl::kFlight;
    return true;
}

void PlumePresenter::present(const u32* pixels, int width, int height, float content_aspect) {
    GEKKO_PROFILE_ZONE("host/present-cpu");
    if (!impl_ || !impl_->ready || pixels == nullptr || width <= 0 || height <= 0) {
        GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
        return;
    }
    Impl& impl = *impl_;

    if (!impl.window_has_area()) {
        ++impl.stats.presents_skipped_minimised;
        GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
        return;
    }
    if (impl.swap_chain->needsResize()) {
        // Same multi-frame stall as the native path: three fence waits, a swap
        // chain resize, and the framebuffer cache dropped (the stale-RTV
        // defect). Once per mouse move during a window drag.
        GEKKO_PROFILE_ZONE("host/present-resize");
        for (int index = 0; index < Impl::kFlight; ++index) {
            impl.wait_slot(impl.slots[index]);
        }
        if (!impl.ready) {
            GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
            return;
        }
        if (!impl.swap_chain->resize()) {
            impl.failed = true;
            impl.ready = false;
            GEKKO_PROFILE_MESSAGE_LITERAL("host/present: swap chain resize failed");
            GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
            return;
        }
        for (int index = 0; index < Impl::kFlight; ++index) {
            impl.slots[index].cleared = false;
        }
        // The blit framebuffer cache below is keyed on the plume
        // RenderTexture*, and that pointer is an element of the swap chain's
        // stable `textures` vector: resize() releases each underlying
        // ID3D12Resource and re-acquires a NEW one into the SAME element, so
        // the pointer compares equal while the RTV descriptor baked into the
        // cached framebuffer still names the released resource. Every present
        // after a resize then cleared and drew into a dead RTV while the
        // barriers and Present acted on the live back buffer. Drop the cache
        // here; it is three objects rebuilt once per resize.
        for (int index = 0; index < Impl::kFlight; ++index) {
            impl.blit_framebuffers[index].reset();
            impl.blit_framebuffer_target[index] = nullptr;
        }
    }
    if (impl.swap_chain->isEmpty()) {
        ++impl.stats.presents_skipped_minimised;  // D06-F5
        GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
        return;
    }

    const u32 surface_width = impl.swap_chain->getWidth();
    const u32 surface_height = impl.swap_chain->getHeight();
    if (surface_width == 0 || surface_height == 0) {
        ++impl.stats.presents_skipped_minimised;
        GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
        return;
    }

    Impl::Slot& slot = impl.slots[impl.slot];
    impl.wait_slot(slot);
    if (!impl.ready) {
        GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
        return;
    }

    std::uint32_t texture_index = 0;
    {
        // Braced only so the zone ends at the acquire; see the native path.
        GEKKO_PROFILE_ZONE("host/present-acquire");
        if (!impl.swap_chain->acquireTexture(nullptr, &texture_index)) {
            impl.failed = true;
            impl.ready = false;
            GEKKO_PROFILE_MESSAGE_LITERAL("host/present: acquireTexture failed");
            GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
            return;
        }
    }
    plume::RenderTexture* back_buffer = impl.swap_chain->getTexture(texture_index);
    if (back_buffer == nullptr) {
        GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
        return;
    }

    const float aspect =
        content_aspect > 0.1f
            ? content_aspect
            : static_cast<float>(width) / static_cast<float>(height);
    const ContentRect dest =
        fit_content_rect(static_cast<int>(surface_width), static_cast<int>(surface_height),
                         aspect);

    // ---- GPU blit ------------------------------------------------------
    // Upload the guest frame at its own size and let the texture unit magnify
    // it, instead of resampling 2.07 million destination pixels per frame on
    // the host job pool. --verify-plume still exercises the CPU resampler,
    // because that is the path its comparison is written against.
    if (impl.blit_ready && !impl.verify) {
        const u32 source_row_pixels = aligned_row_pixels(width);
        const u64 source_required =
            static_cast<u64>(source_row_pixels) * height * sizeof(u32);
        if (slot.upload == nullptr || slot.upload_size < source_required) {
            if (slot.upload != nullptr && slot.mapped != nullptr) {
                slot.upload->unmap();
                slot.mapped = nullptr;
            }
            // A persistently mapped upload buffer per flight slot, rebuilt
            // whenever the frame grows. Nothing tracked these before: at 4K
            // they are ~8.3 MB each and there are three.
            GEKKO_PROFILE_ONLY(if (slot.upload != nullptr) {
                GEKKO_PROFILE_FREE(slot.upload.get(), "present-staging");
                impl.staging_bytes -= slot.upload_size;
            })
            slot.upload =
                impl.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(source_required));
            if (slot.upload == nullptr) {
                impl.failed = true;
                impl.ready = false;
                GEKKO_PROFILE_PLOT("host/present staging bytes", static_cast<double>(impl.staging_bytes));
                return;
            }
            slot.upload_size = source_required;
            GEKKO_PROFILE_ONLY(impl.staging_bytes += slot.upload_size;)
            GEKKO_PROFILE_ALLOC(slot.upload.get(), slot.upload_size, "present-staging");
            GEKKO_PROFILE_PLOT("host/present staging bytes", static_cast<double>(impl.staging_bytes));
            slot.mapped = static_cast<u32*>(slot.upload->map());
            if (slot.mapped == nullptr) {
                impl.failed = true;
                impl.ready = false;
                return;
            }
        }
        if (slot.source == nullptr || slot.source_width != static_cast<u32>(width) ||
            slot.source_height != static_cast<u32>(height)) {
            GEKKO_PROFILE_ONLY(if (slot.source != nullptr) {
                GEKKO_PROFILE_FREE(slot.source.get(), "present-staging");
                impl.staging_bytes -=
                    static_cast<u64>(slot.source_width) * slot.source_height * sizeof(u32);
            })
            slot.source = impl.device->createTexture(plume::RenderTextureDesc::Texture2D(
                static_cast<u32>(width), static_cast<u32>(height), 1,
                plume::RenderFormat::B8G8R8A8_UNORM));
            if (slot.source == nullptr) {
                impl.failed = true;
                impl.ready = false;
                GEKKO_PROFILE_PLOT("host/present staging bytes", static_cast<double>(impl.staging_bytes));
                return;
            }
            slot.source_width = static_cast<u32>(width);
            slot.source_height = static_cast<u32>(height);
            GEKKO_PROFILE_ONLY(impl.staging_bytes +=
                               static_cast<u64>(width) * height * sizeof(u32);)
            GEKKO_PROFILE_ALLOC(slot.source.get(),
                                static_cast<u64>(width) * height * sizeof(u32),
                                "present-staging");
            GEKKO_PROFILE_PLOT("host/present staging bytes", static_cast<double>(impl.staging_bytes));
            slot.source_descriptor->setTexture(0, slot.source.get(),
                                               plume::RenderTextureLayout::SHADER_READ);
            slot.source_descriptor->setTexture(1, slot.source.get(),
                                               plume::RenderTextureLayout::SHADER_READ);
        }
        // One straight row-by-row copy into pitch-aligned upload memory. When
        // the pitch already matches it is a single memcpy.
        if (source_row_pixels == static_cast<u32>(width)) {
            std::memcpy(slot.mapped, pixels,
                        static_cast<std::size_t>(width) * height * sizeof(u32));
        } else {
            for (int y = 0; y < height; ++y) {
                std::memcpy(slot.mapped + static_cast<std::size_t>(y) * source_row_pixels,
                            pixels + static_cast<std::size_t>(y) * width,
                            static_cast<std::size_t>(width) * sizeof(u32));
            }
        }

        if (impl.blit_framebuffer_target[texture_index] != back_buffer ||
            impl.blit_framebuffers[texture_index] == nullptr) {
            const plume::RenderTexture* attachment = back_buffer;
            impl.blit_framebuffers[texture_index] =
                impl.device->createFramebuffer(plume::RenderFramebufferDesc(&attachment, 1));
            impl.blit_framebuffer_target[texture_index] = back_buffer;
            if (impl.blit_framebuffers[texture_index] == nullptr) {
                impl.failed = true;
                impl.ready = false;
                return;
            }
        }

        {
            // Command recording on the GPU-blit path. The brace stops before
            // the `dumping` declaration below rather than at end(), because
            // `dumping` DECREMENTS blit_dump_countdown and is read again after
            // the submit - hoisting it above this zone would move a side
            // effect, and enclosing it would put it out of scope there. What is
            // left outside is the one-shot dump recording, the final PRESENT
            // barrier and end(); the dump itself is covered by
            // host/present-readback.
            GEKKO_PROFILE_ZONE("host/present-record");
            slot.commands->begin();
            slot.commands->barriers(
                plume::RenderBarrierStage::COPY,
                plume::RenderTextureBarrier(slot.source.get(),
                                            plume::RenderTextureLayout::COPY_DEST));
            slot.commands->copyTextureRegion(
                plume::RenderTextureCopyLocation::Subresource(slot.source.get()),
                plume::RenderTextureCopyLocation::PlacedFootprint(
                    slot.upload.get(), plume::RenderFormat::B8G8R8A8_UNORM,
                    static_cast<u32>(width), static_cast<u32>(height), 1, source_row_pixels));
            const plume::RenderTextureBarrier draw_barriers[] = {
                plume::RenderTextureBarrier(slot.source.get(),
                                            plume::RenderTextureLayout::SHADER_READ),
                plume::RenderTextureBarrier(back_buffer,
                                            plume::RenderTextureLayout::COLOR_WRITE),
            };
            slot.commands->barriers(plume::RenderBarrierStage::GRAPHICS, draw_barriers,
                                    static_cast<u32>(std::size(draw_barriers)));
            slot.commands->setFramebuffer(impl.blit_framebuffers[texture_index].get());
            // Clear every present rather than caching the geometry: on the GPU a
            // full-surface clear is essentially free, and it removes the stale-bar
            // failure mode the CPU path had to cache around.
            slot.commands->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
            // Root signature before the pipeline state, matching the order the GX
            // raster path uses.
            slot.commands->setGraphicsPipelineLayout(impl.blit_layout.get());
            slot.commands->setPipeline(impl.blit_pipeline.get());
            slot.commands->setGraphicsDescriptorSet(slot.source_descriptor.get(), 0);
            // The viewport IS the letterbox/pillarbox: the fullscreen triangle maps
            // the whole source across exactly the rectangle fit_content_rect chose.
            const plume::RenderViewport viewport(
                static_cast<float>(dest.x), static_cast<float>(dest.y),
                static_cast<float>(dest.width), static_cast<float>(dest.height));
            slot.commands->setViewports(&viewport, 1);
            const plume::RenderRect scissor(dest.x, dest.y, dest.x + dest.width,
                                            dest.y + dest.height);
            slot.commands->setScissors(&scissor, 1);
            slot.commands->drawInstanced(3, 1, 0, 0);
        }
        const bool dumping = !impl.blit_dump_path.empty() && impl.blit_dump_countdown > 0 &&
                             --impl.blit_dump_countdown == 0;
        const u32 dump_row_pixels = aligned_row_pixels(surface_width);
        if (dumping) {
            const u64 dump_bytes =
                static_cast<u64>(dump_row_pixels) * surface_height * sizeof(u32);
            if (impl.readback == nullptr || impl.readback_size < dump_bytes) {
                GEKKO_PROFILE_ONLY(if (impl.readback != nullptr) {
                    GEKKO_PROFILE_FREE(impl.readback.get(), "present-staging");
                    impl.staging_bytes -= impl.readback_size;
                })
                impl.readback =
                    impl.device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(dump_bytes));
                impl.readback_size = impl.readback != nullptr ? dump_bytes : 0;
                GEKKO_PROFILE_ONLY(if (impl.readback != nullptr) {
                    impl.staging_bytes += impl.readback_size;
                    GEKKO_PROFILE_ALLOC(impl.readback.get(), impl.readback_size,
                                        "present-staging");
                })
                GEKKO_PROFILE_PLOT("host/present staging bytes", static_cast<double>(impl.staging_bytes));
            }
            if (impl.readback != nullptr) {
                slot.commands->barriers(plume::RenderBarrierStage::COPY,
                                        plume::RenderTextureBarrier(
                                            back_buffer,
                                            plume::RenderTextureLayout::COPY_SOURCE));
                auto destination = plume::RenderTextureCopyLocation::PlacedFootprint(
                    impl.readback.get(), plume::RenderFormat::B8G8R8A8_UNORM, surface_width,
                    surface_height, 1, dump_row_pixels);
                destination.texture = back_buffer;
                slot.commands->copyTextureRegion(
                    destination, plume::RenderTextureCopyLocation::Subresource(back_buffer));
            }
        }
        slot.commands->barriers(plume::RenderBarrierStage::NONE,
                                plume::RenderTextureBarrier(
                                    back_buffer, plume::RenderTextureLayout::PRESENT));
        slot.commands->end();
        const plume::RenderCommandList* blit_list = slot.commands.get();
        {
            // Same scope as before; lock_queue only adds the contention
            // counters (D06-F6). The zone includes lock_queue because the
            // collision with the Flipper's own submission is the point.
            GEKKO_PROFILE_ZONE("host/present-submit");
            std::unique_lock<std::mutex> queue_lock = impl.lock_queue();
            impl.queue->executeCommandLists(&blit_list, 1, nullptr, 0, nullptr, 0,
                                            slot.fence.get());
        }
        slot.in_flight = true;
        slot.retirement_outcome = plume::RenderFenceWait::TIMEOUT;
        if (dumping && impl.readback != nullptr) {
            // Blocking wait_slot, a full-surface map and a PPM write on the
            // presenting thread; fires on exactly one frame of a --present-dump
            // run and explains that run's one unexplained hitch.
            GEKKO_PROFILE_ZONE("host/present-readback");
            impl.wait_slot(slot);
            if (!impl.ready) return;
            if (const u32* seen = static_cast<const u32*>(impl.readback->map())) {
                if (std::FILE* file = std::fopen(impl.blit_dump_path.c_str(), "wb")) {
                    std::fprintf(file, "P6\n%u %u\n255\n", surface_width, surface_height);
                    std::vector<unsigned char> row(static_cast<std::size_t>(surface_width) * 3u);
                    for (u32 y = 0; y < surface_height; ++y) {
                        const u32* line = seen + static_cast<std::size_t>(y) * dump_row_pixels;
                        for (u32 x = 0; x < surface_width; ++x) {
                            row[x * 3u + 0u] = static_cast<unsigned char>((line[x] >> 16) & 0xffu);
                            row[x * 3u + 1u] = static_cast<unsigned char>((line[x] >> 8) & 0xffu);
                            row[x * 3u + 2u] = static_cast<unsigned char>(line[x] & 0xffu);
                        }
                        std::fwrite(row.data(), 1, row.size(), file);
                    }
                    std::fclose(file);
                    std::fprintf(stderr, "[present] wrote back-buffer capture %s (%ux%u)\n",
                                 impl.blit_dump_path.c_str(), surface_width, surface_height);
                }
                impl.readback->unmap();
            }
            impl.blit_dump_path.clear();
        }
        {
            // See the native path: the brace has to cover the failure branch
            // because `presented` is declared here and read by it.
            GEKKO_PROFILE_ZONE("host/present-swap");
            const auto present_start = std::chrono::steady_clock::now();  // D06-F5
            const bool presented = impl.swap_chain->present(texture_index, nullptr, 0);
            impl.record_present(present_start);
            if (!presented) {
                ++impl.stats.present_failures;
                ++impl.stats.present_failures_consecutive;
                GEKKO_PROFILE_MESSAGE_LITERAL("host/present: Present() failed");
                GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
                std::fprintf(
                    stderr, "[dx12-present] Present() failed (consecutive %llu)\n",
                    static_cast<unsigned long long>(impl.stats.present_failures_consecutive));
                std::fflush(stderr);
                if (impl.stats.present_failures_consecutive >= 3u) {
                    impl.failed = true;
                    impl.ready = false;
                }
                return;
            }
        }
        impl.stats.present_failures_consecutive = 0;
        ++impl.report.cpu_upload_frames;
        impl.slot = (impl.slot + 1) % Impl::kFlight;
        return;
    }

    // Stage the whole surface so the region outside the frame is defined.
    const u32 row_pixels = aligned_row_pixels(surface_width);
    const u64 required = static_cast<u64>(row_pixels) * surface_height * sizeof(u32);
    if (slot.upload == nullptr || slot.upload_size < required) {
        if (slot.upload != nullptr && slot.mapped != nullptr) {
            slot.upload->unmap();
            slot.mapped = nullptr;
        }
        GEKKO_PROFILE_ONLY(if (slot.upload != nullptr) {
            GEKKO_PROFILE_FREE(slot.upload.get(), "present-staging");
            impl.staging_bytes -= slot.upload_size;
        })
        slot.upload = impl.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(required));
        if (slot.upload == nullptr) {
            impl.failed = true;
            impl.ready = false;
            GEKKO_PROFILE_PLOT("host/present staging bytes", static_cast<double>(impl.staging_bytes));
            return;
        }
        slot.upload_size = required;
        GEKKO_PROFILE_ONLY(impl.staging_bytes += slot.upload_size;)
        GEKKO_PROFILE_ALLOC(slot.upload.get(), slot.upload_size, "present-staging");
        GEKKO_PROFILE_PLOT("host/present staging bytes", static_cast<double>(impl.staging_bytes));
        slot.mapped = static_cast<u32*>(slot.upload->map());
        slot.cleared = false;
        if (slot.mapped == nullptr) {
            impl.failed = true;
            impl.ready = false;
            return;
        }
    }

    const u32 copy_width = dest.width > 0 ? static_cast<u32>(dest.width) : 0u;
    const u32 copy_height = dest.height > 0 ? static_cast<u32>(dest.height) : 0u;
    const u32 offset_x = dest.x > 0 ? static_cast<u32>(dest.x) : 0u;
    const u32 offset_y = dest.y > 0 ? static_cast<u32>(dest.y) : 0u;
    const std::size_t src_stride = static_cast<std::size_t>(width);
    const std::size_t dst_stride = static_cast<std::size_t>(row_pixels);
    const std::size_t row_bytes = static_cast<std::size_t>(copy_width) * sizeof(u32);
    const bool one_to_one = width == dest.width && height == dest.height;

    u32* mapped = slot.mapped;
    if (mapped == nullptr) {
        impl.failed = true;
        impl.ready = false;
        return;
    }
    // Clear only when the staged geometry actually changes. Every present below
    // writes the full content rect, so once the surrounding region has been
    // zeroed for this geometry it stays zeroed. Re-clearing per present cost a
    // full-surface memset into write-combined upload memory every frame.
    const bool clear_geometry_matches =
        slot.cleared && slot.cleared_surface_width == surface_width &&
        slot.cleared_surface_height == surface_height && slot.cleared_x == offset_x &&
        slot.cleared_y == offset_y && slot.cleared_width == copy_width &&
        slot.cleared_height == copy_height;
    if (!clear_geometry_matches) {
        // A full-surface memset into WRITE-COMBINED upload memory - up to
        // 8.3 MB at 4K - run serially on the presenting thread. The comment
        // above records that doing it per present was a measured cost; the
        // remaining per-geometry-change case is still milliseconds, it is
        // invisible today, and it fires once per slot after every resize,
        // which is exactly when the present cadence is already broken.
        GEKKO_PROFILE_ZONE("host/present-stage-clear");
        std::memset(mapped, 0, static_cast<std::size_t>(required));
        slot.cleared = true;
        slot.cleared_surface_width = surface_width;
        slot.cleared_surface_height = surface_height;
        slot.cleared_x = offset_x;
        slot.cleared_y = offset_y;
        slot.cleared_width = copy_width;
        slot.cleared_height = copy_height;
    }
    if (one_to_one && copy_width > 0 && copy_height > 0) {
        auto copy_row = [&](int y) {
            const std::size_t row = static_cast<std::size_t>(y);
            std::memcpy(mapped + (row + offset_y) * dst_stride + offset_x,
                        pixels + row * src_stride, row_bytes);
        };
        if (copy_height >= 64u) {
            host_parallel_for(JobSite::PresentStage, 0, static_cast<int>(copy_height), copy_row);
        } else {
            for (u32 y = 0; y < copy_height; ++y) {
                copy_row(static_cast<int>(y));
            }
        }
    } else if (copy_width > 0 && copy_height > 0) {
        scale_rgba_bilinear(pixels, width, height, mapped, static_cast<int>(row_pixels),
                            static_cast<int>(offset_x), static_cast<int>(offset_y),
                            dest.width, dest.height);
    }

    if (impl.verify && (impl.readback == nullptr || impl.readback_size < required)) {
        GEKKO_PROFILE_ONLY(if (impl.readback != nullptr) {
            GEKKO_PROFILE_FREE(impl.readback.get(), "present-staging");
            impl.staging_bytes -= impl.readback_size;
        })
        impl.readback = impl.device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(required));
        impl.readback_size = impl.readback != nullptr ? required : 0;
        GEKKO_PROFILE_ONLY(if (impl.readback != nullptr) {
            impl.staging_bytes += impl.readback_size;
            GEKKO_PROFILE_ALLOC(impl.readback.get(), impl.readback_size, "present-staging");
        })
        GEKKO_PROFILE_PLOT("host/present staging bytes", static_cast<double>(impl.staging_bytes));
    }
    const bool verifying = impl.verify && impl.readback != nullptr;

    {
        // Command recording only. The staging memset and the row copy above are
        // NOT in here - they run before begin() - and are covered by
        // host/present-stage-clear and the PresentStage job site instead.
        GEKKO_PROFILE_ZONE("host/present-record");
        slot.commands->begin();
        slot.commands->barriers(plume::RenderBarrierStage::COPY,
                                plume::RenderTextureBarrier(back_buffer,
                                                            plume::RenderTextureLayout::COPY_DEST));
        slot.commands->copyTextureRegion(
            plume::RenderTextureCopyLocation::Subresource(back_buffer),
            plume::RenderTextureCopyLocation::PlacedFootprint(
                slot.upload.get(), plume::RenderFormat::B8G8R8A8_UNORM, surface_width, surface_height,
                1, row_pixels));
        if (verifying) {
            slot.commands->barriers(plume::RenderBarrierStage::COPY,
                                    plume::RenderTextureBarrier(
                                        back_buffer, plume::RenderTextureLayout::COPY_SOURCE));
            auto destination = plume::RenderTextureCopyLocation::PlacedFootprint(
                impl.readback.get(), plume::RenderFormat::B8G8R8A8_UNORM, surface_width,
                surface_height, 1, row_pixels);
            // plume's Direct3D 12 command list reads the destination location's
            // texture pointer to decide sample positions before it looks at the
            // copy type, so a buffer destination - which leaves that pointer null -
            // faults. It is not read for a placed footprint otherwise, and the
            // sample positions that matter are the source surface's, so naming the
            // source there is both harmless and correct.
            destination.texture = back_buffer;
            slot.commands->copyTextureRegion(
                destination, plume::RenderTextureCopyLocation::Subresource(back_buffer));
        }
        slot.commands->barriers(plume::RenderBarrierStage::NONE,
                                plume::RenderTextureBarrier(back_buffer,
                                                            plume::RenderTextureLayout::PRESENT));
        slot.commands->end();
    }

    const plume::RenderCommandList* list = slot.commands.get();
    {
        // Same scope as before; lock_queue only adds the contention counters
        // (D06-F6). The zone includes lock_queue: the wait is the interesting
        // part, and it is where this thread and the Flipper collide.
        GEKKO_PROFILE_ZONE("host/present-submit");
        std::unique_lock<std::mutex> queue_lock = impl.lock_queue();
        impl.queue->executeCommandLists(&list, 1, nullptr, 0, nullptr, 0,
                                        slot.fence.get());
    }
    slot.in_flight = true;
    slot.retirement_outcome = plume::RenderFenceWait::TIMEOUT;

    // The oracle only covers the 1:1 staging path. Every other surface size -
    // including the DEFAULT window - takes the resampled or GPU-blit path and
    // was reported as "0 frames, 0 differed", which reads as a pass. Count what
    // is skipped so an unverified run is visibly unverified.
    if (impl.verify && !one_to_one) ++impl.report.skipped;
    if (verifying && one_to_one) {
        // Blocking wait_slot, a full-surface map and a per-pixel compare on the
        // presenting thread. This is why a --verify-plume run's numbers are not
        // comparable with anything else.
        GEKKO_PROFILE_ZONE("host/present-readback");
        impl.wait_slot(slot);
        if (!impl.ready) return;
        if (const auto* seen = static_cast<const u32*>(impl.readback->map())) {
            ++impl.report.frames;
            u64 bad = 0;
            for (u32 y = 0; y < copy_height; ++y) {
                const u32* expected = pixels + static_cast<std::size_t>(y) * static_cast<u32>(width);
                const u32* actual =
                    seen + static_cast<std::size_t>(y + offset_y) * row_pixels + offset_x;
                for (u32 x = 0; x < copy_width; ++x) {
                    // The swap chain carries no alpha, so only the colour the
                    // screen shows is compared.
                    if ((expected[x] & 0x00ffffffu) != (actual[x] & 0x00ffffffu)) {
                        if (bad == 0 && impl.report.mismatched_pixels == 0) {
                            impl.report.first_bad_x = x;
                            impl.report.first_bad_y = y;
                            impl.report.first_bad_expected = expected[x];
                            impl.report.first_bad_actual = actual[x];
                        }
                        ++bad;
                    }
                }
            }
            if (bad != 0) {
                ++impl.report.mismatched_frames;
                impl.report.mismatched_pixels += bad;
            }
            impl.readback->unmap();
        }
    }

    {
        // See the native path: the brace has to cover the failure branch
        // because `presented` is declared here and read by it.
        GEKKO_PROFILE_ZONE("host/present-swap");
        const auto present_start = std::chrono::steady_clock::now();  // D06-F5
        const bool presented = impl.swap_chain->present(texture_index, nullptr, 0);
        impl.record_present(present_start);
        if (!presented) {
            ++impl.stats.present_failures;
            ++impl.stats.present_failures_consecutive;
            GEKKO_PROFILE_MESSAGE_LITERAL("host/present: Present() failed");
            GEKKO_PROFILE_PLOT("host/present refusals", static_cast<double>(++g_present_refusals));
            std::fprintf(
                stderr, "[dx12-present] Present() failed (consecutive %llu)\n",
                static_cast<unsigned long long>(impl.stats.present_failures_consecutive));
            std::fflush(stderr);
            if (impl.stats.present_failures_consecutive >= 3u) {
                impl.failed = true;
                impl.ready = false;
            }
            return;
        }
    }
    impl.stats.present_failures_consecutive = 0;
    ++impl.report.cpu_upload_frames;
    impl.slot = (impl.slot + 1) % Impl::kFlight;
}

}  // namespace gekko

#else  // SHADOW_ENABLE_PLUME

namespace gekko {

struct PlumePresenter::Impl {};

PlumePresenter::PlumePresenter() = default;
PlumePresenter::~PlumePresenter() = default;
bool PlumePresenter::create(void*) { return false; }
bool PlumePresenter::create(void*, void*, void*, void*) { return false; }
void PlumePresenter::destroy() {}
bool PlumePresenter::available() const noexcept { return false; }
bool PlumePresenter::failed() const noexcept { return true; }
bool PlumePresenter::fence_failed() const noexcept { return false; }
std::string PlumePresenter::last_error() const { return {}; }
void PlumePresenter::present(const u32*, int, int, float) {}
bool PlumePresenter::present_native(void*, void*, float, int, int, float, const NativePin*,
                                    const NativePin*) {
    return false;
}
void PlumePresenter::set_verify(bool) noexcept {}
void PlumePresenter::set_gpu_blit(bool) noexcept {}
void PlumePresenter::set_area_resolve(bool) noexcept {}
void PlumePresenter::set_vsync(bool) noexcept {}
// Signature drift: the header has taken a frame ordinal since the acceptance
// capture stopped being hardcoded to 30, and this branch was never updated.
void PlumePresenter::set_present_dump(const char*, int) {}
void PlumePresenter::set_narrow_queue_lock(bool) noexcept {}
void PlumePresenter::set_native_release_hook(NativeReleaseHook, void*) noexcept {}
void PlumePresenter::set_deferred_pin_release(bool) noexcept {}
bool PlumePresenter::deferred_pin_release_active() const noexcept { return false; }
void PlumePresenter::retire_completed_presents() {}
PlumePresenter::VerifyReport PlumePresenter::verify_report() const { return VerifyReport{}; }
PlumePresenter::PresentStats PlumePresenter::present_stats() const { return PresentStats{}; }

}  // namespace gekko

#endif  // SHADOW_ENABLE_PLUME
