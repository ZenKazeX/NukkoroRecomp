#pragma once

// Uploads into GPU-only resources on a dedicated COPY queue, expressed through
// the RHI (plume's abstract interfaces) alone. No backend type crosses this
// interface or appears in its implementation, so it serves D3D12, Vulkan and
// Metal unchanged. It replaces hw::GpuRuntime, which drove a raw D3D12 copy
// queue beside plume and kept a second, pointer-keyed resource-state table.
//
// Contract:
//   * A batch owns its staging memory. The CPU writes a batch's staging only
//     after that batch's previous submission has retired, so no copy can read
//     bytes a later upload overwrote. (The 2026-09-21 copy-list ring shared one
//     staging ring that every open rewound, and corrupted geometry.)
//   * Uploads are recorded into the open batch. submit() executes it and hands
//     back the semaphore the consuming GRAPHICS submission must wait on. The
//     renderer submits the batch immediately before each GRAPHICS list, so a
//     list never runs before the copies it reads, and a resource released while
//     that list is in flight dies after the copy that wrote it.
//   * Only resources with no earlier GPU use may be written here. A copy queue
//     cannot order itself behind GRAPHICS work that still reads the old
//     contents; rewrites of a live resource belong on the GRAPHICS list.
//   * The RHI's strict fence and semaphore contract is followed on every
//     backend: a fence is waited exactly once per submission that signalled
//     it and never otherwise (Vulkan resets on wait), and a semaphore is
//     re-signalled only after the submission that waited on it has retired
//     (binary semaphores). The renderer returns consumed semaphores with
//     retire().
//   * Exhaustion is bounded and counted. An upload larger than one batch's
//     staging is refused (the caller stages it on the GRAPHICS list); a full
//     batch is submitted early and the next opened, blocking only if that
//     batch is still in flight.

#include <memory>
#include <string>
#include <vector>

#include "gekko/types.h"

namespace plume {
struct RenderDevice;
struct RenderBuffer;
struct RenderTexture;
struct RenderCommandSemaphore;
}  // namespace plume

namespace gekko::render {

// Row pitch and placement of texels in staging memory: the strictest of the
// three backends (D3D12's 256-byte pitch and 512-byte placement), which the
// other two accept as ordinary strides and offsets.
constexpr u64 kStagingRowAlignment = 256;
constexpr u64 kStagingPlacementAlignment = 512;

class TransferQueue {
public:
    enum class UploadOwner : u8 { Texture, GeometryVertex, GeometryIndex, ImmediateIndex, Count };
    using BufferWriter = void (*)(void* destination, u64 bytes, void* context);
    struct Config {
        u64 staging_bytes = 8ull << 20;  // per batch
        u64 minimum_chunk_bytes = 64ull << 10;
        u32 batches = 2;
    };

    struct Stats {
        u64 submits = 0;
        u64 early_submits = 0;     // a batch filled before the GRAPHICS list closed
        u64 batch_waits = 0;       // reuses of a submitted batch (each waits its fence once)
        u64 batch_wait_ns = 0;     // ... and the time those waits took: non-zero = real blocking
        u64 buffer_uploads = 0;
        u64 generated_buffer_uploads = 0;  // direct producer writes, no source memcpy
        u64 generated_buffer_bytes = 0;
        u64 texture_uploads = 0;
        u64 bytes = 0;             // payload bytes (not padded staging)
        u64 staging_bytes = 0;     // staging consumed, including row and placement padding
        u64 retained_bytes = 0;    // mapped staging retained by all batches
        u64 peak_batch_bytes = 0;  // largest used prefix of one submitted batch
        u64 largest_request_bytes = 0;
        u64 growths = 0;
        u64 allocation_failures = 0;
        u64 current_bytes = 0;
        u64 resets = 0;
        u64 chunk_switches = 0;
        u64 owner_uploads[static_cast<std::size_t>(UploadOwner::Count)]{};
        u64 owner_bytes[static_cast<std::size_t>(UploadOwner::Count)]{};
        u64 owner_staging_bytes[static_cast<std::size_t>(UploadOwner::Count)]{};
        u64 refused_arguments = 0; // null or zero-sized request
        u64 refused_oversize = 0;  // larger than one batch's staging
        u64 semaphores = 0;        // created over the session (pool high water)
        u64 semaphore_failures = 0; // none could be created: the CPU waited the copy instead
    };

    TransferQueue();
    ~TransferQueue();
    TransferQueue(const TransferQueue&) = delete;
    TransferQueue& operator=(const TransferQueue&) = delete;

    bool create(plume::RenderDevice& device, const Config& config, std::string& error);
    // Waits for every batch in flight, then releases everything.
    void destroy();
    bool live() const noexcept;

    // Records a copy of `bytes` from `data` into `dest` at `dest_offset`. False
    // when refused (counted); nothing is recorded then.
    bool upload_buffer(plume::RenderBuffer& dest, u64 dest_offset, const void* data, u64 bytes,
                       UploadOwner owner);
    // Reserves mapped staging and lets the producer write the final bytes
    // there once. This is the FastGeo-style direct-fill path for generated
    // resources: no temporary CPU vector and no second memcpy into staging.
    bool upload_buffer_generated(plume::RenderBuffer& dest, u64 dest_offset, u64 bytes,
                                 UploadOwner owner, BufferWriter writer, void* context);
    // Records a copy of a tightly packed RGBA8 image into mip 0 of `dest`.
    bool upload_texture_rgba8(plume::RenderTexture& dest, u32 width, u32 height, const void* rgba);
    // True when uploads are recorded that no submit() has executed yet.
    bool pending() const noexcept;

    // Executes the open batch, if any, and appends to `waits` the semaphore of
    // every batch executed since the previous call - early submits included.
    // The next GRAPHICS submission must wait on all of them, and hand them back
    // through retire() once it has itself retired.
    void submit(std::vector<plume::RenderCommandSemaphore*>& waits);
    void retire(const std::vector<plume::RenderCommandSemaphore*>& semaphores);

    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gekko::render
