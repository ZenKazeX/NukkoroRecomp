// TransferQueue: COPY-queue uploads through the RHI. See gpu_transfer.h for
// the contract. Everything here is plume's abstract interface; there is no
// backend type in this file.

#include "gekko/render/gpu_transfer.h"
#include "gekko/render/upload_arena.h"
#include "gekko/render/copy_dependency_publication.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <exception>
#include <optional>

#if defined(SHADOW_ENABLE_PLUME)
#include "plume_render_interface.h"
#endif

namespace gekko::render {

#if !defined(SHADOW_ENABLE_PLUME)

struct TransferQueue::Impl {};
TransferQueue::TransferQueue() : impl_(std::make_unique<Impl>()) {}
TransferQueue::~TransferQueue() = default;
bool TransferQueue::create(plume::RenderDevice&, const Config&, std::string& error) {
    error = "built without plume";
    return false;
}
void TransferQueue::destroy() {}
bool TransferQueue::live() const noexcept { return false; }
bool TransferQueue::upload_buffer(plume::RenderBuffer&, u64, const void*, u64, UploadOwner) { return false; }
bool TransferQueue::upload_buffer_generated(plume::RenderBuffer&, u64, u64, UploadOwner,
                                            BufferWriter, void*) { return false; }
bool TransferQueue::upload_texture_rgba8(plume::RenderTexture&, u32, u32, const void*) { return false; }
bool TransferQueue::pending() const noexcept { return false; }
void TransferQueue::submit(std::vector<plume::RenderCommandSemaphore*>&) {}
void TransferQueue::retire(const std::vector<plume::RenderCommandSemaphore*>&) {}
TransferQueue::Stats TransferQueue::stats() const { return {}; }

#else

namespace {

constexpr u64 align_up(u64 value, u64 alignment) noexcept {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

}  // namespace

struct TransferQueue::Impl {
    struct Batch {
        std::unique_ptr<plume::RenderCommandList> list;
        std::unique_ptr<plume::RenderCommandFence> fence;
        UploadArena staging;
        bool open = false;       // list recording, staging being written
        bool in_flight = false;  // submitted; fence signalled on completion, not yet waited
    };

    plume::RenderDevice* device = nullptr;
    std::unique_ptr<plume::RenderCommandQueue> queue;
    std::vector<Batch> batches;
    u32 current = 0;
    u64 staging_bytes = 0;
    // Semaphores are pooled, not tied to a batch: one is re-signalled only after
    // the GRAPHICS submission that waited on it has retired (retire()).
    std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> semaphores;
    std::vector<plume::RenderCommandSemaphore*> free_semaphores;
    std::vector<plume::RenderCommandSemaphore*> signalled;  // since the last submit() hand-off
    Stats stats;
    bool live = false;

    void retire_batch(Batch& batch) {
        if (!batch.in_flight) return;
        const auto result = plume::waitForRetirement([&](u64 timeout) {
            return queue->waitForCommandFenceFor(batch.fence.get(), timeout);
        });
        plume::completeRetirement(result, "native COPY batch", [&] { batch.in_flight = false; });
    }

    plume::RenderCommandSemaphore* take_semaphore() {
        if (!free_semaphores.empty()) {
            plume::RenderCommandSemaphore* semaphore = free_semaphores.back();
            free_semaphores.pop_back();
            return semaphore;
        }
        auto created = device->createCommandSemaphore();
        if (created == nullptr) return nullptr;
        semaphores.push_back(std::move(created));
        ++stats.semaphores;
        return semaphores.back().get();
    }

    // The open batch, opening the current one if needed. Opening waits for the
    // batch's previous submission: that is the only point at which its staging
    // bytes may be rewritten.
    Batch& open_batch() {
        Batch& batch = batches[current];
        if (batch.open) return batch;
        if (batch.in_flight) {
            // The RHI has no non-blocking fence query, so every reuse of a batch
            // that was submitted waits; the time says whether any of them blocked.
            const auto begin = std::chrono::steady_clock::now();
            retire_batch(batch);
            stats.batch_wait_ns += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                       std::chrono::steady_clock::now() - begin)
                                                       .count());
            ++stats.batch_waits;
        }
        batch.staging.reset();
        batch.list->begin();
        batch.open = true;
        return batch;
    }

    void execute(Batch& batch) {
        batch.list->end();
        batch.open = false;
        plume::RenderCommandSemaphore* semaphore = take_semaphore();
        // Acquire storage before COPY executes; successful publication below
        // cannot allocate and lose the consuming GRAPHICS dependency.
        std::optional<CopyDependencyPublication<decltype(signalled)>> publication;
        if (semaphore != nullptr) publication.emplace(signalled);
        const plume::RenderCommandList* lists[] = {batch.list.get()};
        plume::RenderCommandSemaphore* signal[] = {semaphore};
        queue->executeCommandLists(lists, 1, nullptr, 0, semaphore != nullptr ? signal : nullptr,
                                   semaphore != nullptr ? 1u : 0u, batch.fence.get());
        batch.in_flight = true;
        if (semaphore != nullptr) {
            publication->publish(semaphore);
        } else {
            // No semaphore means no GPU-side dependency for the GRAPHICS list,
            // so the CPU provides it: the copy completes before anything that
            // reads it can be submitted. Counted, because it serialises.
            ++stats.semaphore_failures;
            retire_batch(batch);
        }
        ++stats.submits;
        current = (current + 1u) % static_cast<u32>(batches.size());
    }

    // `bytes` of staging at `alignment` in the open batch; a full batch is
    // executed early and the next one opened. Null only for an oversize request.
    UploadArena::Allocation stage(u64 bytes, u64 alignment, UploadOwner owner) {
        stats.largest_request_bytes = (std::max)(stats.largest_request_bytes, bytes);
        if (bytes > staging_bytes) {
            ++stats.refused_oversize;
            return {};
        }
        Batch* batch = &open_batch();
        u64 used_before = batch->staging.used_bytes();
        UploadArena::Allocation allocation = batch->staging.allocate(bytes, alignment);
        if (!allocation && used_before != 0u) {
            ++stats.early_submits;
            execute(*batch);
            batch = &open_batch();
            used_before = batch->staging.used_bytes();
            allocation = batch->staging.allocate(bytes, alignment);
        }
        if (!allocation) return {};
        const u64 used_after = batch->staging.used_bytes();
        stats.peak_batch_bytes = (std::max)(stats.peak_batch_bytes, used_after);
        stats.staging_bytes += used_after - used_before;
        const std::size_t owner_index = static_cast<std::size_t>(owner);
        ++stats.owner_uploads[owner_index];
        stats.owner_bytes[owner_index] += bytes;
        stats.owner_staging_bytes[owner_index] += used_after - used_before;
        return allocation;
    }
};

TransferQueue::TransferQueue() : impl_(std::make_unique<Impl>()) {}
TransferQueue::~TransferQueue() { destroy(); }

bool TransferQueue::create(plume::RenderDevice& device, const Config& config, std::string& error) {
    destroy();
    Impl& impl = *impl_;
    impl.device = &device;
    impl.staging_bytes = config.staging_bytes;
    impl.queue = device.createCommandQueue(plume::RenderCommandListType::COPY);
    if (impl.queue == nullptr) {
        error = "COPY queue";
        return false;
    }
    impl.batches.resize(std::max<u32>(1u, config.batches));
    for (Impl::Batch& batch : impl.batches) {
        batch.list = impl.queue->createCommandList();
        batch.fence = device.createCommandFence();
        UploadArena::Config arena_config;
        arena_config.minimum_chunk_bytes = config.minimum_chunk_bytes;
        arena_config.max_bytes = config.staging_bytes;
        arena_config.committed = true;
        if (batch.list == nullptr || batch.fence == nullptr ||
            !batch.staging.create(device, arena_config, error)) {
            error = "COPY batch objects";
            destroy();
            return false;
        }
    }
    impl.live = true;
    return true;
}

void TransferQueue::destroy() {
    if (!impl_) return;
    Impl& impl = *impl_;
    for (Impl::Batch& batch : impl.batches) {
        if (batch.open) {
            // Recorded but never executed: close it so the list can be released.
            batch.list->end();
            batch.open = false;
        }
        if (batch.in_flight && impl.queue != nullptr) {
            try { impl.retire_batch(batch); }
            catch (const plume::FenceRetirementError& error) {
                if (error.outcome() != plume::RenderFenceWait::DEVICE_LOST) {
                    std::fprintf(stderr, "[native-copy] fatal teardown before staging release: %s\n", error.what());
                    std::fflush(stderr);
                    std::terminate();
                }
                // Confirmed removed device no longer consumes these buffers.
                batch.in_flight = false;
            }
        }
        batch.staging.destroy();
    }
    impl.batches.clear();
    impl.signalled.clear();
    impl.free_semaphores.clear();
    impl.semaphores.clear();
    impl.queue.reset();
    impl.device = nullptr;
    impl.live = false;
}

bool TransferQueue::live() const noexcept { return impl_ && impl_->live; }

bool TransferQueue::upload_buffer(plume::RenderBuffer& dest, u64 dest_offset, const void* data,
                                  u64 bytes, UploadOwner owner) {
    Impl& impl = *impl_;
    if (!impl.live || data == nullptr || bytes == 0u) {
        ++impl.stats.refused_arguments;
        return false;
    }
    const UploadArena::Allocation staging = impl.stage(bytes, 16u, owner);
    if (!staging) return false;
    std::memcpy(staging.data, data, static_cast<std::size_t>(bytes));
    Impl::Batch& batch = impl.batches[impl.current];
    batch.list->copyBufferRegion(plume::RenderBufferReference(&dest, dest_offset),
                                 plume::RenderBufferReference(staging.buffer, staging.offset), bytes);
    ++impl.stats.buffer_uploads;
    impl.stats.bytes += bytes;
    return true;
}

bool TransferQueue::upload_buffer_generated(plume::RenderBuffer& dest, u64 dest_offset, u64 bytes,
                                            UploadOwner owner, BufferWriter writer, void* context) {
    Impl& impl = *impl_;
    if (!impl.live || writer == nullptr || bytes == 0u) {
        ++impl.stats.refused_arguments;
        return false;
    }
    const UploadArena::Allocation staging = impl.stage(bytes, 16u, owner);
    if (!staging) return false;
    writer(staging.data, bytes, context);
    Impl::Batch& batch = impl.batches[impl.current];
    batch.list->copyBufferRegion(plume::RenderBufferReference(&dest, dest_offset),
                                 plume::RenderBufferReference(staging.buffer, staging.offset), bytes);
    ++impl.stats.buffer_uploads;
    ++impl.stats.generated_buffer_uploads;
    impl.stats.generated_buffer_bytes += bytes;
    impl.stats.bytes += bytes;
    return true;
}

bool TransferQueue::upload_texture_rgba8(plume::RenderTexture& dest, u32 width, u32 height,
                                         const void* rgba) {
    Impl& impl = *impl_;
    if (!impl.live || rgba == nullptr || width == 0u || height == 0u) {
        ++impl.stats.refused_arguments;
        return false;
    }
    const u64 row_bytes = static_cast<u64>(width) * 4u;
    const u64 pitch = align_up(row_bytes, kStagingRowAlignment);
    const UploadArena::Allocation staging =
        impl.stage(pitch * height, kStagingPlacementAlignment, UploadOwner::Texture);
    if (!staging) return false;
    const auto* source = static_cast<const u8*>(rgba);
    for (u32 y = 0; y < height; ++y) {
        std::memcpy(staging.data + pitch * y, source + row_bytes * y,
                    static_cast<std::size_t>(row_bytes));
    }
    Impl::Batch& batch = impl.batches[impl.current];
    batch.list->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(&dest),
        plume::RenderTextureCopyLocation::PlacedFootprint(staging.buffer,
                                                          plume::RenderFormat::R8G8B8A8_UNORM, width,
                                                          height, 1, static_cast<u32>(pitch / 4u),
                                                          staging.offset));
    ++impl.stats.texture_uploads;
    impl.stats.bytes += row_bytes * height;
    return true;
}

bool TransferQueue::pending() const noexcept {
    const Impl& impl = *impl_;
    return impl.live && !impl.batches.empty() && impl.batches[impl.current].open;
}

void TransferQueue::submit(std::vector<plume::RenderCommandSemaphore*>& waits) {
    Impl& impl = *impl_;
    if (!impl.live) return;
    Impl::Batch& batch = impl.batches[impl.current];
    if (batch.open) impl.execute(batch);
    waits.insert(waits.end(), impl.signalled.begin(), impl.signalled.end());
    impl.signalled.clear();
}

void TransferQueue::retire(const std::vector<plume::RenderCommandSemaphore*>& semaphores) {
    Impl& impl = *impl_;
    impl.free_semaphores.insert(impl.free_semaphores.end(), semaphores.begin(), semaphores.end());
}

TransferQueue::Stats TransferQueue::stats() const {
    Stats out = impl_->stats;
    out.retained_bytes = 0;
    out.growths = 0;
    out.allocation_failures = 0;
    out.current_bytes = 0;
    out.resets = 0;
    out.chunk_switches = 0;
    for (const Impl::Batch& batch : impl_->batches) {
        const UploadArena::Stats arena = batch.staging.stats();
        out.retained_bytes += arena.retained_bytes;
        out.growths += arena.growths;
        out.allocation_failures += arena.allocation_failures;
        out.current_bytes += arena.used_bytes;
        out.resets += arena.resets;
        out.chunk_switches += arena.chunk_switches;
    }
    return out;
}

#endif

}  // namespace gekko::render
