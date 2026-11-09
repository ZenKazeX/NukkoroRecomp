#pragma once

// The new native bloom system's GPU side.
//
// Owns every resource the effect needs and records its passes. It shares
// nothing with the material pipeline: its own root signature, its own
// descriptor sets, its own render targets. That separation is deliberate - the
// bloom is a self-contained subsystem, and a change here must not be able to
// perturb the draw path.
//
// The mathematics live in gekko/bloom.h and are validated against a CPU
// reference in runtime/tests/bloom_tests.cpp. This class renders those
// formulas; where the two could disagree, the reference is authoritative.
//
// Resource discipline, which is a requirement and not an aspiration:
//   * five textures, sized from the frame by bloom::level_sizes, never from the
//     internal render scale, never from a guest raster, never from a literal;
//   * every descriptor allocated when the level set is built and never written
//     again - a steady-state frame performs zero descriptor writes and zero
//     allocations;
//   * transitions are the eight the pass order requires, all known statically;
//   * no copies. The scene is composited into in place, so the original's
//     full-screen snapshot and its add-back do not exist here.
//
// Plume types are passed as void* so this header does not drag the backend into
// everything that includes it; the implementation casts them back. Same
// convention as gekko/native_gpu.h.

#include <memory>
#include <string>

#include "gekko/bloom.h"
#include "gekko/types.h"

namespace gekko {

class BloomRenderer {
public:
    BloomRenderer();
    ~BloomRenderer();

    BloomRenderer(const BloomRenderer&) = delete;
    BloomRenderer& operator=(const BloomRenderer&) = delete;

    // True when this build carries the Direct3D 12 backend.
    static bool available() noexcept;

    // `device` is a plume::RenderDevice*. Builds the shaders, the root
    // signature, the samplers and the pipelines; no render target is allocated
    // until the first resize.
    bool create(void* device, std::string& error);
    void destroy();

    // Sizes the level set for a frame of `width` x `height`. Cheap and
    // idempotent when the derived sizes have not changed, which is the common
    // case across a window drag: the levels follow the ASPECT, so most resizes
    // do not move them at all. Returns false only if the device refused an
    // allocation, in which case the caller must not record any pass.
    //
    // Old textures are handed to `release` (a callback into the caller's
    // deferred-release list) rather than destroyed, because a command list in
    // flight may still reference them.
    using ReleaseFn = void (*)(void* context, void* texture, void* framebuffer);
    bool resize(u32 width, u32 height, u32 quality, ReleaseFn release, void* release_context);

    struct Frame {
        // The glare source: what the guest's bucket-5, land _G_/_GL_ and lens
        // flare draws rendered into. W x H, in SHADER_READ.
        void* glare_source = nullptr;
        // The scene colour target and its framebuffer; the composite blends
        // into this with ONE/ONE.
        void* scene = nullptr;
        void* scene_framebuffer = nullptr;
        u32 width = 0;
        u32 height = 0;
    };

    // Records the whole chain onto `command_list` (a plume::RenderCommandList*).
    // Does nothing when the gate is closed, which is the guest's own condition.
    // Returns the number of passes recorded.
    u32 record(void* command_list, const Frame& frame, const bloom::Parameters& parameters);

    struct Stats {
        u64 frames = 0;
        u64 passes = 0;
        u64 level_rebuilds = 0;
        u64 allocation_failures = 0;
        u64 gate_closed_frames = 0;
        // The live level set, for the propagation report.
        u32 level_a_width = 0;
        u32 level_a_height = 0;
        u32 level_b_width = 0;
        u32 level_b_height = 0;
        u64 target_bytes = 0;
    };
    Stats stats() const;

    // One line for the exit report, describing the level set and what it cost.
    std::string report() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gekko
