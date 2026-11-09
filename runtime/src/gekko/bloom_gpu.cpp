#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "gekko/bloom_gpu.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "gekko/profiling.h"

#if defined(SHADOW_ENABLE_PLUME)
#include "bloom_shader_ps_blur.h"
#include "bloom_shader_ps_capture.h"
#include "bloom_shader_ps_composite.h"
#include "bloom_shader_ps_merge.h"
#include "bloom_shader_vs.h"
#include "plume_d3d12.h"
#endif

namespace gekko {

#if !defined(SHADOW_ENABLE_PLUME)

struct BloomRenderer::Impl {};
BloomRenderer::BloomRenderer() : impl_(std::make_unique<Impl>()) {}
BloomRenderer::~BloomRenderer() = default;
bool BloomRenderer::available() noexcept { return false; }
bool BloomRenderer::create(void*, std::string& error) {
    error = "plume (Direct3D 12) was not compiled in";
    return false;
}
void BloomRenderer::destroy() {}
bool BloomRenderer::resize(u32, u32, u32, ReleaseFn, void*) { return false; }
u32 BloomRenderer::record(void*, const Frame&, const bloom::Parameters&) { return 0; }
BloomRenderer::Stats BloomRenderer::stats() const { return {}; }
std::string BloomRenderer::report() const { return {}; }

#else

namespace {

// UNORM8 everywhere. The 8-bit saturation is behavioural: level B's total gain
// is 4.7059, so it clips once the glare source passes about 54/255, and that
// clipping is the look. A float chain would silently remove it.
constexpr plume::RenderFormat kLevelFormat = plume::RenderFormat::R8G8B8A8_UNORM;

// The constant block the bloom shaders read. Mirrors `cbuffer BloomConstants`
// in bloom_shaders.hlsl; the two must not drift.
struct alignas(16) BloomConstants {
    float dst_size[4];  // w, h, 1/w, 1/h
    float src_size[4];
    float gain = 0.0f;
    u32 tap_count = 1;
    u32 tap_axis = 0;
    u32 pad0 = 0;
    float taps[16]{};
};
// HLSL packs a constant buffer into 16-byte registers: float4 dst_size (16) +
// float4 src_size (16) + the {gain, tap_count, tap_axis, pad} register (16) +
// float4 taps[4] (64) = 112. The C++ layout above must come to the same.
static_assert(sizeof(BloomConstants) == 112u,
              "BloomConstants must match the HLSL cbuffer layout");

// Which two textures a pass reads. One descriptor set per distinct pair, all
// built when the level set is built, so a steady-state frame writes none.
enum class Binding : u32 {
    CaptureA = 0,   // t0 = glare source
    BlurAH,         // t1 = level A
    BlurAV,         // t1 = level A ping
    CaptureB,       // t0 = level A
    BlurBH,         // t1 = level B
    BlurBV,         // t1 = level B ping
    Merge,          // t0 = level A, t1 = level B
    Composite,      // t0 = glare source, t1 = level A ping
    Count,
};

struct Target {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;
    u32 width = 0;
    u32 height = 0;
    // Tracked so a transition is emitted only when the layout actually changes.
    plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;

    void reset() {
        framebuffer.reset();
        texture.reset();
        width = height = 0;
        layout = plume::RenderTextureLayout::UNKNOWN;
    }
};

bool usable(const std::unique_ptr<plume::RenderTexture>& texture) {
    return texture != nullptr && static_cast<plume::D3D12Texture*>(texture.get())->d3d != nullptr;
}

// How the three bloom plots are drawn. Declared from whichever of resize() or
// record() runs first, through a function-local static; without Tracy neither
// the function nor the static guard exists, so the shipped binary keeps the
// steady-state path it has today.
GEKKO_PROFILE_ONLY(void configure_bloom_plots() {
    GEKKO_PROFILE_PLOT_CONFIG("gpu/bloom passes", GEKKO_PLOT_NUMBER, true, true, 0x4FA8FFu);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/bloom level bytes", GEKKO_PLOT_MEMORY, true, true, 0xFFB03Fu);
    GEKKO_PROFILE_PLOT_CONFIG("gpu/bloom allocation failures", GEKKO_PLOT_NUMBER, true, true,
                              0xFF4040u);
})

}  // namespace

struct BloomRenderer::Impl {
    plume::RenderDevice* device = nullptr;

    std::unique_ptr<plume::RenderShader> vs;
    std::unique_ptr<plume::RenderShader> ps_capture;
    std::unique_ptr<plume::RenderShader> ps_blur;
    std::unique_ptr<plume::RenderShader> ps_merge;
    std::unique_ptr<plume::RenderShader> ps_composite;
    std::unique_ptr<plume::RenderSampler> sampler_point;
    std::unique_ptr<plume::RenderSampler> sampler_linear;
    std::unique_ptr<plume::RenderPipelineLayout> layout;
    std::unique_ptr<plume::RenderPipeline> pipeline_capture;
    std::unique_ptr<plume::RenderPipeline> pipeline_blur;
    std::unique_ptr<plume::RenderPipeline> pipeline_merge;
    // The composite blends ONE/ONE onto the scene instead of writing it, which
    // is why the scene never has to be copied out and added back.
    std::unique_ptr<plume::RenderPipeline> pipeline_composite;

    // The level set. GlareSource and the scene belong to the caller.
    Target level_a, level_a_ping, level_b, level_b_ping;
    bloom::LevelSizes sizes;
    u32 frame_width = 0;
    u32 frame_height = 0;
    u32 quality = 1;

    std::array<std::unique_ptr<plume::RenderDescriptorSet>, static_cast<u32>(Binding::Count)> sets;
    // The textures each set currently points at, so a rebuild rewrites only
    // what moved.
    std::array<std::array<const plume::RenderTexture*, 2>, static_cast<u32>(Binding::Count)> bound{};

    // Persistently mapped constants, one block per pass per frame in flight.
    static constexpr u32 kPasses = 9;
    static constexpr u32 kFlight = 3;
    std::unique_ptr<plume::RenderBuffer> constants;
    u8* constants_mapped = nullptr;
    u32 constants_cursor = 0;

    mutable std::mutex stats_mutex;
    BloomRenderer::Stats stats;

    plume::RenderDescriptorSetDesc set_desc() const {
        static const plume::RenderDescriptorRange ranges[] = {
            plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 2),
        };
        return plume::RenderDescriptorSetDesc(ranges, 1);
    }

    bool create(std::string& error) {
        vs = device->createShader(kBloomVSBloom, sizeof(kBloomVSBloom), "VSBloom",
                                  plume::RenderShaderFormat::DXIL);
        ps_capture = device->createShader(kBloomPSBloomCapture, sizeof(kBloomPSBloomCapture),
                                          "PSBloomCapture", plume::RenderShaderFormat::DXIL);
        ps_blur = device->createShader(kBloomPSBloomBlur, sizeof(kBloomPSBloomBlur), "PSBloomBlur",
                                       plume::RenderShaderFormat::DXIL);
        ps_merge = device->createShader(kBloomPSBloomMerge, sizeof(kBloomPSBloomMerge),
                                        "PSBloomMerge", plume::RenderShaderFormat::DXIL);
        ps_composite = device->createShader(kBloomPSBloomComposite, sizeof(kBloomPSBloomComposite),
                                            "PSBloomComposite", plume::RenderShaderFormat::DXIL);
        if (!vs || !ps_capture || !ps_blur || !ps_merge || !ps_composite) {
            error = "bloom shader creation failed";
            return false;
        }

        // Two samplers, both CLAMP. The zero border is enforced in the shader by
        // rejecting out-of-range taps, not by a border colour: a border colour
        // would also apply to the merge and composite magnifications, where
        // CLAMP is what the original's addressing did.
        plume::RenderSamplerDesc point;
        point.minFilter = plume::RenderFilter::NEAREST;
        point.magFilter = plume::RenderFilter::NEAREST;
        point.mipmapMode = plume::RenderMipmapMode::NEAREST;
        point.addressU = plume::RenderTextureAddressMode::CLAMP;
        point.addressV = plume::RenderTextureAddressMode::CLAMP;
        point.addressW = plume::RenderTextureAddressMode::CLAMP;
        point.maxLOD = 0.0f;
        sampler_point = device->createSampler(point);

        plume::RenderSamplerDesc linear = point;
        linear.minFilter = plume::RenderFilter::LINEAR;
        linear.magFilter = plume::RenderFilter::LINEAR;
        sampler_linear = device->createSampler(linear);
        if (!sampler_point || !sampler_linear) {
            error = "bloom sampler creation failed";
            return false;
        }

        const plume::RenderSampler* immutable[] = {sampler_point.get(), sampler_linear.get()};
        const plume::RenderDescriptorRange texture_range(
            plume::RenderDescriptorRangeType::TEXTURE, 0, 2);
        const plume::RenderDescriptorRange sampler_range(
            plume::RenderDescriptorRangeType::SAMPLER, 0, 2, immutable);
        const plume::RenderDescriptorRange ranges[] = {texture_range, sampler_range};
        const plume::RenderDescriptorSetDesc desc(ranges, 2);
        const plume::RenderRootDescriptorDesc root(0, 2,
                                                   plume::RenderRootDescriptorType::CONSTANT_BUFFER);
        plume::RenderPipelineLayoutDesc layout_desc;
        layout_desc.descriptorSetDescs = &desc;
        layout_desc.descriptorSetDescsCount = 1;
        layout_desc.rootDescriptorDescs = &root;
        layout_desc.rootDescriptorDescsCount = 1;
        layout_desc.allowInputLayout = false;
        layout = device->createPipelineLayout(layout_desc);
        if (layout == nullptr) {
            error = "bloom pipeline layout";
            return false;
        }

        plume::RenderGraphicsPipelineDesc pipeline;
        pipeline.pipelineLayout = layout.get();
        pipeline.vertexShader = vs.get();
        pipeline.renderTargetFormat[0] = kLevelFormat;
        pipeline.renderTargetCount = 1;
        pipeline.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
        pipeline.cullMode = plume::RenderCullMode::NONE;
        pipeline.depthEnabled = false;
        pipeline.depthWriteEnabled = false;
        pipeline.inputSlotsCount = 0;
        pipeline.inputElementsCount = 0;

        pipeline.pixelShader = ps_capture.get();
        pipeline_capture = device->createGraphicsPipeline(pipeline);
        pipeline.pixelShader = ps_blur.get();
        pipeline_blur = device->createGraphicsPipeline(pipeline);
        pipeline.pixelShader = ps_merge.get();
        pipeline_merge = device->createGraphicsPipeline(pipeline);

        // The composite is the only pass that blends: it ADDS onto the scene.
        pipeline.pixelShader = ps_composite.get();
        pipeline.renderTargetBlend[0].blendEnabled = true;
        pipeline.renderTargetBlend[0].srcBlend = plume::RenderBlend::ONE;
        pipeline.renderTargetBlend[0].dstBlend = plume::RenderBlend::ONE;
        pipeline.renderTargetBlend[0].blendOp = plume::RenderBlendOperation::ADD;
        pipeline.renderTargetBlend[0].srcBlendAlpha = plume::RenderBlend::ONE;
        pipeline.renderTargetBlend[0].dstBlendAlpha = plume::RenderBlend::ONE;
        pipeline.renderTargetBlend[0].blendOpAlpha = plume::RenderBlendOperation::ADD;
        pipeline_composite = device->createGraphicsPipeline(pipeline);

        if (!pipeline_capture || !pipeline_blur || !pipeline_merge || !pipeline_composite) {
            error = "bloom pipeline creation failed";
            return false;
        }

        const u64 constant_bytes = static_cast<u64>(sizeof(BloomConstants)) * kPasses * kFlight;
        constants = device->createBuffer(
            plume::RenderBufferDesc::UploadBuffer(constant_bytes,
                                                  plume::RenderBufferFlag::CONSTANT));
        if (constants == nullptr) {
            error = "bloom constant buffer";
            return false;
        }
        constants_mapped = static_cast<u8*>(constants->map());
        if (constants_mapped == nullptr) {
            error = "bloom constant buffer could not be mapped";
            return false;
        }

        for (auto& set : sets) {
            set = device->createDescriptorSet(desc);
            if (set == nullptr) {
                error = "bloom descriptor set";
                return false;
            }
        }
        return true;
    }

    bool build_target(Target& target, u32 width, u32 height, ReleaseFn release, void* context) {
        if (target.width == width && target.height == height && usable(target.texture)) {
            return true;
        }
        if (target.texture != nullptr && release != nullptr) {
            // The pool's FREE lives here and in destroy(), never in
            // Target::reset(): this line hands the raw pointer to the deferred
            // release BEFORE reset() runs, so a free inside reset() would
            // double-report on this path and miss the destroy path entirely.
            GEKKO_PROFILE_FREE(target.texture.get(), "GPU bloom levels");
            release(context, target.texture.release(), target.framebuffer.release());
        }
        target.reset();
        const plume::RenderClearValue clear =
            plume::RenderClearValue::Color(plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f), kLevelFormat);
        target.texture = device->createTexture(plume::RenderTextureDesc::ColorTarget(
            width, height, kLevelFormat, plume::RenderMultisampling(), &clear));
        if (!usable(target.texture)) {
            target.reset();
            ++stats.allocation_failures;
            // Until now this counter only surfaced in report() at end of run,
            // with no time axis, so a failure on one resize that recovered on
            // the next was indistinguishable from one that persisted. The step
            // lands on the timeline next to the gpu/bloom passes drop it causes.
            GEKKO_PROFILE_PLOT("gpu/bloom allocation failures",
                               static_cast<double>(stats.allocation_failures));
            return false;
        }
        const plume::RenderTexture* attachment = target.texture.get();
        target.framebuffer =
            device->createFramebuffer(plume::RenderFramebufferDesc(&attachment, 1));
        if (target.framebuffer == nullptr) {
            target.reset();
            ++stats.allocation_failures;
            GEKKO_PROFILE_PLOT("gpu/bloom allocation failures",
                               static_cast<double>(stats.allocation_failures));
            return false;
        }
        target.width = width;
        target.height = height;
        target.layout = plume::RenderTextureLayout::UNKNOWN;
        // Four render targets behind a deferred release callback is the shape
        // that leaks silently, and this pool is the only thing here that still
        // tells the truth after a resize that failed halfway through.
        GEKKO_PROFILE_ALLOC(target.texture.get(),
                            static_cast<std::size_t>(width) * height * 4u, "GPU bloom levels");
        return true;
    }
};

BloomRenderer::BloomRenderer() : impl_(std::make_unique<Impl>()) {}
BloomRenderer::~BloomRenderer() { destroy(); }
bool BloomRenderer::available() noexcept { return true; }

bool BloomRenderer::create(void* device, std::string& error) {
    if (device == nullptr) {
        error = "bloom: no device";
        return false;
    }
    impl_->device = static_cast<plume::RenderDevice*>(device);
    return impl_->create(error);
}

void BloomRenderer::destroy() {
    if (!impl_ || impl_->device == nullptr) return;
    // The other half of the pool. The `device == nullptr` return above is only
    // reachable after a failed create(nullptr) or a second destroy, so nothing
    // allocated ever escapes without a matching free.
    GEKKO_PROFILE_ONLY(for (const Target* level :
                            {&impl_->level_a, &impl_->level_a_ping, &impl_->level_b,
                             &impl_->level_b_ping}) {
        if (level->texture != nullptr) {
            GEKKO_PROFILE_FREE(level->texture.get(), "GPU bloom levels");
        }
    })
    impl_->level_a.reset();
    impl_->level_a_ping.reset();
    impl_->level_b.reset();
    impl_->level_b_ping.reset();
    for (auto& set : impl_->sets) set.reset();
    if (impl_->constants != nullptr && impl_->constants_mapped != nullptr) {
        impl_->constants->unmap();
        impl_->constants_mapped = nullptr;
    }
    impl_->constants.reset();
    impl_->pipeline_capture.reset();
    impl_->pipeline_blur.reset();
    impl_->pipeline_merge.reset();
    impl_->pipeline_composite.reset();
    impl_->layout.reset();
    impl_->sampler_point.reset();
    impl_->sampler_linear.reset();
    impl_->vs.reset();
    impl_->ps_capture.reset();
    impl_->ps_blur.reset();
    impl_->ps_merge.reset();
    impl_->ps_composite.reset();
    impl_->device = nullptr;
}

bool BloomRenderer::resize(u32 width, u32 height, u32 quality, ReleaseFn release,
                           void* release_context) {
    GEKKO_PROFILE_ONLY(static const bool plots_configured = (configure_bloom_plots(), true);
                       (void)plots_configured;)
    Impl& impl = *impl_;
    if (impl.device == nullptr) return false;
    const bloom::LevelSizes sizes = bloom::level_sizes(width, height, quality);
    if (!sizes.valid()) return false;
    impl.frame_width = width;
    impl.frame_height = height;
    impl.quality = quality;
    // The levels follow the ASPECT, so most window drags do not move them at
    // all and this is a no-op. That is the point of rule R1.
    if (sizes.a_width == impl.sizes.a_width && sizes.a_height == impl.sizes.a_height &&
        sizes.b_width == impl.sizes.b_width && sizes.b_height == impl.sizes.b_height &&
        usable(impl.level_a.texture)) {
        return true;
    }
    if (!impl.build_target(impl.level_a, sizes.a_width, sizes.a_height, release, release_context) ||
        !impl.build_target(impl.level_a_ping, sizes.a_width, sizes.a_height, release,
                           release_context) ||
        !impl.build_target(impl.level_b, sizes.b_width, sizes.b_height, release, release_context) ||
        !impl.build_target(impl.level_b_ping, sizes.b_width, sizes.b_height, release,
                           release_context)) {
        return false;
    }
    impl.sizes = sizes;
    ++impl.stats.level_rebuilds;
    impl.stats.level_a_width = sizes.a_width;
    impl.stats.level_a_height = sizes.a_height;
    impl.stats.level_b_width = sizes.b_width;
    impl.stats.level_b_height = sizes.b_height;
    const u64 a = static_cast<u64>(sizes.a_width) * sizes.a_height * 4u;
    const u64 b = static_cast<u64>(sizes.b_width) * sizes.b_height * 4u;
    impl.stats.target_bytes = 2u * a + 2u * b;
    // Reads the field resize() has just computed, so no new work. The levels
    // follow the ASPECT and not the internal scale, which makes a flat line
    // across a window drag with a step only when the aspect moves the cheapest
    // possible test of rule R1. It is NOT ground truth on its own: a
    // build_target that fails partway returns above this line, leaving the
    // stale size plotted against a half-new level set. The GPU bloom levels
    // pool is the one that cannot lie.
    GEKKO_PROFILE_PLOT("gpu/bloom level bytes", static_cast<double>(impl.stats.target_bytes));
    // Every descriptor the chain uses is written here and never again.
    std::fill(impl.bound.begin(), impl.bound.end(),
              std::array<const plume::RenderTexture*, 2>{nullptr, nullptr});
    return true;
}

u32 BloomRenderer::record(void* command_list, const Frame& frame,
                          const bloom::Parameters& parameters) {
    Impl& impl = *impl_;
    if (impl.device == nullptr || command_list == nullptr) return 0u;
    auto* list = static_cast<plume::RenderCommandList*>(command_list);
    auto* glare_source = static_cast<plume::RenderTexture*>(frame.glare_source);
    auto* scene_framebuffer = static_cast<plume::RenderFramebuffer*>(frame.scene_framebuffer);
    if (glare_source == nullptr || scene_framebuffer == nullptr) return 0u;

    GEKKO_PROFILE_ONLY(static const bool plots_configured = (configure_bloom_plots(), true);
                       (void)plots_configured;)
    // The outer bracket for one frame of bloom recording: the guest gate, the
    // ladder and the composite. A gate-closed frame shows up as a near-empty
    // zone, which is itself the answer to "is bloom running at all".
    GEKKO_PROFILE_ZONE("gpu/bloom-record");

    ++impl.stats.frames;
    // The guest's own gate. When it is closed the original returns early from
    // both entry points and the glare-source draws land on the scene instead;
    // there is nothing for this chain to do.
    if (!bloom::gate_open(parameters)) {
        ++impl.stats.gate_closed_frames;
        GEKKO_PROFILE_PLOT("gpu/bloom passes", 0.0);
        return 0u;
    }
    if (!usable(impl.level_a.texture) || !usable(impl.level_b.texture)) {
        // An explicit zero matters here. This return fires when a level target
        // failed to build, and a stepped plot with no point would hold the last
        // good value - so a dead level set would read as a healthy frame, which
        // is exactly the failure the plot exists to make visible.
        GEKKO_PROFILE_PLOT("gpu/bloom passes", 0.0);
        return 0u;
    }

    std::vector<float> taps;

    const auto to_read = [&](Target& target) {
        if (target.layout == plume::RenderTextureLayout::SHADER_READ) return;
        list->barriers(plume::RenderBarrierStage::GRAPHICS,
                       plume::RenderTextureBarrier(target.texture.get(),
                                                   plume::RenderTextureLayout::SHADER_READ));
        target.layout = plume::RenderTextureLayout::SHADER_READ;
    };
    const auto to_write = [&](Target& target) {
        if (target.layout == plume::RenderTextureLayout::COLOR_WRITE) return;
        list->barriers(plume::RenderBarrierStage::GRAPHICS,
                       plume::RenderTextureBarrier(target.texture.get(),
                                                   plume::RenderTextureLayout::COLOR_WRITE));
        target.layout = plume::RenderTextureLayout::COLOR_WRITE;
    };
    // Descriptors are written only when the texture behind a slot actually
    // moves, which happens on a level rebuild and never in a steady-state frame.
    const auto bind = [&](Binding slot, const plume::RenderTexture* t0,
                          const plume::RenderTexture* t1) {
        const u32 index = static_cast<u32>(slot);
        auto& current = impl.bound[index];
        plume::RenderDescriptorSet* set = impl.sets[index].get();
        if (set == nullptr) return set;
        if (current[0] != t0) {
            set->setTexture(0, t0, plume::RenderTextureLayout::SHADER_READ);
            current[0] = t0;
        }
        if (current[1] != t1) {
            set->setTexture(1, t1, plume::RenderTextureLayout::SHADER_READ);
            current[1] = t1;
        }
        return set;
    };

    u32 recorded = 0;
    const auto pass = [&](plume::RenderPipeline* pipeline, plume::RenderDescriptorSet* set,
                          plume::RenderFramebuffer* destination, u32 dst_w, u32 dst_h,
                          u32 src_w, u32 src_h, float gain, u32 axis,
                          const std::vector<float>& offsets) {
        if (pipeline == nullptr || set == nullptr || destination == nullptr) return;
        // One zone per recorded pass, nested inside the level zones, covering
        // the constant memcpy plus the framebuffer/viewport/scissor/pipeline/
        // descriptor-set/draw sequence. The destination extent is what tells
        // the rungs apart, and carrying it as a value keeps the zone name
        // stable - eight separate names would buy nothing the enclosing level
        // zones do not already separate.
        GEKKO_PROFILE_ZONE("gpu/bloom-pass");
        GEKKO_PROFILE_VALUE((static_cast<u64>(dst_w) << 16) | dst_h);
        BloomConstants c{};
        c.dst_size[0] = static_cast<float>(dst_w);
        c.dst_size[1] = static_cast<float>(dst_h);
        c.dst_size[2] = 1.0f / static_cast<float>(dst_w);
        c.dst_size[3] = 1.0f / static_cast<float>(dst_h);
        c.src_size[0] = static_cast<float>(src_w);
        c.src_size[1] = static_cast<float>(src_h);
        c.src_size[2] = 1.0f / static_cast<float>(src_w);
        c.src_size[3] = 1.0f / static_cast<float>(src_h);
        c.gain = gain;
        c.tap_axis = axis;
        c.tap_count = static_cast<u32>(std::min<std::size_t>(offsets.size(), 16u));
        for (u32 i = 0; i < c.tap_count; ++i) c.taps[i] = offsets[i];

        const u32 slot = impl.constants_cursor % (Impl::kPasses * Impl::kFlight);
        impl.constants_cursor = slot + 1u;
        const u64 offset = static_cast<u64>(slot) * sizeof(BloomConstants);
        std::memcpy(impl.constants_mapped + offset, &c, sizeof(c));

        list->setFramebuffer(destination);
        const plume::RenderViewport viewport(0.0f, 0.0f, static_cast<float>(dst_w),
                                             static_cast<float>(dst_h), 0.0f, 1.0f);
        const plume::RenderRect scissor(0, 0, static_cast<int32_t>(dst_w),
                                        static_cast<int32_t>(dst_h));
        list->setViewports(&viewport, 1);
        list->setScissors(&scissor, 1);
        list->setPipeline(pipeline);
        list->setGraphicsPipelineLayout(impl.layout.get());
        list->setGraphicsDescriptorSet(set, 0);
        list->setGraphicsRootDescriptor(
            plume::RenderBufferReference(impl.constants.get(), offset), 0);
        list->drawInstanced(3, 1, 0, 0);
        ++recorded;
    };

    const u32 lw = impl.sizes.a_width, lh = impl.sizes.a_height;
    const u32 bw = impl.sizes.b_width, bh = impl.sizes.b_height;
    const std::vector<float> none;

    // ---- P2: capture level A from the glare source ------------------------
    // Rung A is the expensive one - 96*quality*aspect x 128*quality - and the
    // rung whose tap count the guest varies per stage, so it carries the
    // level's texel count. The zone brackets the WHOLE branch, not its body: a
    // frame where the guest disabled A then reads as a near-empty zone rather
    // than as a missing one.
    {
        GEKKO_PROFILE_ZONE("gpu/bloom-level-a");
        GEKKO_PROFILE_VALUE(static_cast<u64>(lw) * lh);
        if (parameters.enable_a) {
            to_write(impl.level_a);
            pass(impl.pipeline_capture.get(), bind(Binding::CaptureA, glare_source, glare_source),
                 impl.level_a.framebuffer.get(), lw, lh, frame.width, frame.height,
                 bloom::capture_gain_a(parameters), 0u, none);

            // ---- P3/P4: the separable halves, H before V ------------------
            bloom::tap_offsets(parameters.taps_ah, parameters.spread_a, taps);
            to_read(impl.level_a);
            to_write(impl.level_a_ping);
            pass(impl.pipeline_blur.get(),
                 bind(Binding::BlurAH, impl.level_a.texture.get(), impl.level_a.texture.get()),
                 impl.level_a_ping.framebuffer.get(), lw, lh, lw, lh, 1.0f, 0u, taps);

            bloom::tap_offsets(parameters.taps_av, parameters.spread_a, taps);
            to_read(impl.level_a_ping);
            to_write(impl.level_a);
            pass(impl.pipeline_blur.get(),
                 bind(Binding::BlurAV, impl.level_a_ping.texture.get(),
                      impl.level_a_ping.texture.get()),
                 impl.level_a.framebuffer.get(), lw, lh, lw, lh, 1.0f, 1u, taps);
        }
    }

    // ---- P5..P7: level B --------------------------------------------------
    // Kept separate from A because this rung carries the behavioural branch:
    // `from_source` captures from the glare source, while the cascade arm
    // captures from level A and adds a to_read(level_a) barrier the source arm
    // does not. Split this way the two arms can be compared without a rebuild.
    {
        GEKKO_PROFILE_ZONE("gpu/bloom-level-b");
        GEKKO_PROFILE_VALUE(static_cast<u64>(bw) * bh);
        if (parameters.enable_b) {
            const bool from_source = parameters.b_from_source || !parameters.enable_a;
            to_write(impl.level_b);
            if (from_source) {
                pass(impl.pipeline_capture.get(),
                     bind(Binding::CaptureB, glare_source, glare_source),
                     impl.level_b.framebuffer.get(), bw, bh, frame.width, frame.height,
                     bloom::capture_gain(parameters.intensity_b, true), 0u, none);
            } else {
                // The cascade arm takes the RAW intensity byte: the 0.95 that
                // level A applies is absent here in the original, and that
                // asymmetry is reproduced deliberately.
                to_read(impl.level_a);
                pass(impl.pipeline_capture.get(),
                     bind(Binding::CaptureB, impl.level_a.texture.get(),
                          impl.level_a.texture.get()),
                     impl.level_b.framebuffer.get(), bw, bh, lw, lh,
                     bloom::capture_gain(parameters.intensity_b, false), 0u, none);
            }

            bloom::tap_offsets(parameters.taps_bh, parameters.spread_b, taps);
            to_read(impl.level_b);
            to_write(impl.level_b_ping);
            pass(impl.pipeline_blur.get(),
                 bind(Binding::BlurBH, impl.level_b.texture.get(), impl.level_b.texture.get()),
                 impl.level_b_ping.framebuffer.get(), bw, bh, bw, bh, 1.0f, 0u, taps);

            bloom::tap_offsets(parameters.taps_bv, parameters.spread_b, taps);
            to_read(impl.level_b_ping);
            to_write(impl.level_b);
            pass(impl.pipeline_blur.get(),
                 bind(Binding::BlurBV, impl.level_b_ping.texture.get(),
                      impl.level_b_ping.texture.get()),
                 impl.level_b.framebuffer.get(), bw, bh, bw, bh, 1.0f, 1u, taps);
        }
    }

    // ---- P8: merge B into A ----------------------------------------------
    // The merge selection and the composite are the only two passes at FRAME
    // resolution rather than level resolution, and the composite is the only
    // one that blends ONE/ONE onto the scene - so this is the pixel-proportional
    // rung the internal scale multiplies. Unlike A and B it is unconditional,
    // which makes this zone the floor of the chain's cost.
    {
        GEKKO_PROFILE_ZONE("gpu/bloom-composite");
        Target* composite_level = &impl.level_a;
        if (parameters.enable_a && parameters.enable_b && !parameters.separate) {
            to_read(impl.level_a);
            to_read(impl.level_b);
            to_write(impl.level_a_ping);
            pass(impl.pipeline_merge.get(),
                 bind(Binding::Merge, impl.level_a.texture.get(), impl.level_b.texture.get()),
                 impl.level_a_ping.framebuffer.get(), lw, lh, lw, lh, 1.0f, 0u, none);
            composite_level = &impl.level_a_ping;
        } else if (!parameters.enable_a) {
            composite_level = &impl.level_b;
        }

        // ---- P9: composite onto the scene, in place -----------------------
        to_read(*composite_level);
        pass(impl.pipeline_composite.get(),
             bind(Binding::Composite, glare_source, composite_level->texture.get()),
             scene_framebuffer, frame.width, frame.height, frame.width, frame.height, 1.0f, 0u,
             none);
    }

    impl.stats.passes += recorded;
    // The per-frame `recorded` local, not the cumulative total: 0 when the
    // guest gate is closed, 8 in the full configuration, 5 when one level is
    // enabled - a direct frame-by-frame readout of which rungs the guest asked
    // for.
    GEKKO_PROFILE_PLOT("gpu/bloom passes", static_cast<double>(recorded));
    return recorded;
}

BloomRenderer::Stats BloomRenderer::stats() const {
    std::lock_guard lock(impl_->stats_mutex);
    return impl_->stats;
}

std::string BloomRenderer::report() const {
    const Stats s = stats();
    if (s.frames == 0u && s.level_rebuilds == 0u) return {};
    char line[320];
    std::snprintf(line, sizeof(line),
                  "[bloom] frames %llu passes %llu | level A %ux%u level B %ux%u"
                  " (%.3f MiB) rebuilds %llu gate-closed %llu allocation failures %llu\n",
                  static_cast<unsigned long long>(s.frames),
                  static_cast<unsigned long long>(s.passes), s.level_a_width, s.level_a_height,
                  s.level_b_width, s.level_b_height,
                  static_cast<double>(s.target_bytes) / (1024.0 * 1024.0),
                  static_cast<unsigned long long>(s.level_rebuilds),
                  static_cast<unsigned long long>(s.gate_closed_frames),
                  static_cast<unsigned long long>(s.allocation_failures));
    return line;
}

#endif  // SHADOW_ENABLE_PLUME

}  // namespace gekko
