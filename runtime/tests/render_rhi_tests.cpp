// The RHI contract the native renderer relies on, against a real device:
//   * render::TransferQueue: COPY-queue uploads, read back through a DIRECT list
//     that waits on the batch semaphores exactly as the renderer's GRAPHICS
//     submissions do;
//   * bounded fence waits (poll, timeout, signalled-exactly-once);
//   * fallible factories (null on failure, never a wrapper around nothing);
//   * the memory budget query.
// No device: skip code 77.

#include "gekko/render/gpu_transfer.h"

#include "plume_d3d12.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using gekko::u32;
using gekko::u64;
using gekko::u8;
using gekko::render::TransferQueue;

int g_failures = 0;

void check(bool condition, int line, const char* text) {
    if (!condition) {
        ++g_failures;
        std::printf("    CHECK failed (line %d): %s\n", line, text);
    }
}
#define CHECK(condition) check(static_cast<bool>(condition), __LINE__, #condition)

struct Device {
    std::unique_ptr<plume::RenderInterface> api;
    std::unique_ptr<plume::RenderDevice> device;
    std::unique_ptr<plume::RenderCommandQueue> direct;
};

bool open_device(Device& out) {
    auto d3d12 = std::make_unique<plume::D3D12Interface>();
    if (!d3d12->isValid()) return false;
    out.api = std::move(d3d12);
    out.device = out.api->createDevice();
    if (out.device == nullptr) return false;
    out.direct = out.device->createCommandQueue(plume::RenderCommandListType::DIRECT);
    return out.direct != nullptr;
}

std::vector<u8> pattern(std::size_t bytes, u32 seed) {
    std::vector<u8> data(bytes);
    u32 x = seed * 2654435761u + 1u;
    for (u8& b : data) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b = static_cast<u8>(x);
    }
    return data;
}

struct GeneratedPattern {
    u32 seed = 0;
};

void fill_generated_pattern(void* destination, u64 bytes, void* opaque) {
    const auto* context = static_cast<const GeneratedPattern*>(opaque);
    const std::vector<u8> generated = pattern(static_cast<std::size_t>(bytes), context->seed);
    std::memcpy(destination, generated.data(), generated.size());
}

// Runs `record` on a DIRECT list that waits on `waits`, blocks until it
// retires, then returns the semaphores to the transfer queue - the renderer's
// submission contract in miniature.
template <typename Record>
void run_direct(Device& d, TransferQueue& transfer, std::vector<plume::RenderCommandSemaphore*>& waits,
                Record&& record) {
    auto list = d.direct->createCommandList();
    auto fence = d.device->createCommandFence();
    list->begin();
    record(*list);
    list->end();
    const plume::RenderCommandList* lists[] = {list.get()};
    d.direct->executeCommandLists(lists, 1, waits.empty() ? nullptr : waits.data(),
                                  static_cast<u32>(waits.size()), nullptr, 0, fence.get());
    d.direct->waitForCommandFence(fence.get());
    transfer.retire(waits);
    waits.clear();
}

bool read_back(Device& d, TransferQueue& transfer, std::vector<plume::RenderCommandSemaphore*>& waits,
               plume::RenderBuffer& buffer, std::size_t bytes, const std::vector<u8>& expected) {
    auto readback = d.device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(bytes));
    run_direct(d, transfer, waits, [&](plume::RenderCommandList& list) {
        list.barriers(plume::RenderBarrierStage::COPY,
                      plume::RenderBufferBarrier(&buffer, plume::RenderBufferAccess::READ));
        list.copyBufferRegion(plume::RenderBufferReference(readback.get()),
                              plume::RenderBufferReference(&buffer), bytes);
    });
    const auto* mapped = static_cast<const u8*>(readback->map());
    const bool equal = mapped != nullptr && std::memcmp(mapped, expected.data(), bytes) == 0;
    readback->unmap();
    return equal;
}

void test_buffer_round_trip(Device& d) {
    std::printf("buffer round trip\n");
    TransferQueue transfer;
    std::string error;
    CHECK(transfer.create(*d.device, TransferQueue::Config{}, error));
    const std::size_t bytes = 48u * 1000u + 12u;
    auto dest = d.device->createBuffer(plume::RenderBufferDesc::DefaultBuffer(bytes));
    const std::vector<u8> data = pattern(bytes, 1);
    CHECK(!transfer.pending());
    CHECK(transfer.upload_buffer(*dest, 0, data.data(), bytes,
                                 TransferQueue::UploadOwner::GeometryVertex));
    CHECK(transfer.pending());
    std::vector<plume::RenderCommandSemaphore*> waits;
    transfer.submit(waits);
    CHECK(!transfer.pending());
    CHECK(waits.size() == 1u);
    CHECK(read_back(d, transfer, waits, *dest, bytes, data));
    const TransferQueue::Stats s = transfer.stats();
    CHECK(s.submits == 1u && s.buffer_uploads == 1u && s.bytes == bytes);
    // Nothing recorded: nothing to wait on.
    transfer.submit(waits);
    CHECK(waits.empty());

    // A producer can write final bytes straight into retained mapped staging,
    // with the same COPY/fence ownership as an ordinary source upload.
    auto generated_dest = d.device->createBuffer(plume::RenderBufferDesc::DefaultBuffer(bytes));
    GeneratedPattern generated_context{77u};
    const std::vector<u8> generated_expected = pattern(bytes, generated_context.seed);
    CHECK(transfer.upload_buffer_generated(*generated_dest, 0, bytes,
                                           TransferQueue::UploadOwner::ImmediateIndex,
                                           fill_generated_pattern,
                                           &generated_context));
    transfer.submit(waits);
    CHECK(read_back(d, transfer, waits, *generated_dest, bytes, generated_expected));
    const TransferQueue::Stats generated_stats = transfer.stats();
    const std::size_t owner =
        static_cast<std::size_t>(TransferQueue::UploadOwner::ImmediateIndex);
    CHECK(generated_stats.owner_uploads[owner] == 1u);
    CHECK(generated_stats.owner_bytes[owner] == bytes);
}

void test_texture_round_trip(Device& d) {
    std::printf("texture round trip (odd width, padded rows)\n");
    TransferQueue transfer;
    std::string error;
    CHECK(transfer.create(*d.device, TransferQueue::Config{}, error));
    const u32 width = 37, height = 5;
    auto texture = d.device->createTexture(
        plume::RenderTextureDesc::Texture2D(width, height, 1, plume::RenderFormat::R8G8B8A8_UNORM));
    const std::vector<u8> rgba = pattern(width * height * 4u, 2);
    CHECK(transfer.upload_texture_rgba8(*texture, width, height, rgba.data()));
    std::vector<plume::RenderCommandSemaphore*> waits;
    transfer.submit(waits);
    const u64 pitch = (width * 4u + 255u) & ~255u;
    auto readback = d.device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(pitch * height));
    run_direct(d, transfer, waits, [&](plume::RenderCommandList& list) {
        list.barriers(plume::RenderBarrierStage::COPY,
                      plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::COPY_SOURCE));
        list.copyTextureRegion(
            plume::RenderTextureCopyLocation::PlacedFootprint(readback.get(),
                                                              plume::RenderFormat::R8G8B8A8_UNORM, width,
                                                              height, 1, static_cast<u32>(pitch / 4u)),
            plume::RenderTextureCopyLocation::Subresource(texture.get()));
    });
    const auto* mapped = static_cast<const u8*>(readback->map());
    bool equal = mapped != nullptr;
    for (u32 y = 0; equal && y < height; ++y) {
        equal = std::memcmp(mapped + pitch * y, rgba.data() + width * 4u * y, width * 4u) == 0;
    }
    readback->unmap();
    CHECK(equal);
    CHECK(transfer.stats().staging_bytes == pitch * height);
}

void test_bounds_and_early_submit(Device& d) {
    std::printf("bounds, early submit, batch reuse\n");
    TransferQueue transfer;
    TransferQueue::Config config;
    config.staging_bytes = 64u << 10;
    config.batches = 2;
    std::string error;
    CHECK(transfer.create(*d.device, config, error));
    std::vector<plume::RenderCommandSemaphore*> waits;

    // Larger than a batch: refused, nothing recorded.
    auto big = d.device->createBuffer(plume::RenderBufferDesc::DefaultBuffer(128u << 10));
    const std::vector<u8> huge = pattern(128u << 10, 3);
    CHECK(!transfer.upload_buffer(*big, 0, huge.data(), huge.size(),
                                  TransferQueue::UploadOwner::GeometryVertex));
    CHECK(transfer.stats().refused_oversize == 1u);
    CHECK(!transfer.pending());
    // Zero bytes (an empty index list): refused.
    CHECK(!transfer.upload_buffer(*big, 0, huge.data(), 0u,
                                  TransferQueue::UploadOwner::GeometryVertex));
    CHECK(transfer.stats().refused_arguments == 1u);

    // Three 40 KiB uploads into 64 KiB batches: the second fills the first
    // batch and submits it early; all three must still arrive intact, and one
    // hand-off must carry both semaphores.
    std::vector<std::unique_ptr<plume::RenderBuffer>> dests;
    std::vector<std::vector<u8>> datas;
    for (u32 i = 0; i < 3u; ++i) {
        dests.push_back(d.device->createBuffer(plume::RenderBufferDesc::DefaultBuffer(40u << 10)));
        datas.push_back(pattern(40u << 10, 10u + i));
        CHECK(transfer.upload_buffer(*dests.back(), 0, datas.back().data(), datas.back().size(),
                                     TransferQueue::UploadOwner::GeometryVertex));
    }
    transfer.submit(waits);
    CHECK(transfer.stats().early_submits == 2u);
    CHECK(waits.size() == 3u);
    for (u32 i = 0; i < 3u; ++i) {
        std::vector<plume::RenderCommandSemaphore*> none;
        if (i == 0) none = waits;  // the first read back waits on (and retires) all three
        CHECK(read_back(d, transfer, none, *dests[i], datas[i].size(), datas[i]));
    }
    waits.clear();

    // Many rounds through two batches: batches are reused only after they
    // retire, semaphores are recycled, data stays intact.
    for (u32 round = 0; round < 8u; ++round) {
        const std::vector<u8> data = pattern(30u << 10, 100u + round);
        CHECK(transfer.upload_buffer(*dests[round % 3u], 0, data.data(), data.size(),
                                     TransferQueue::UploadOwner::GeometryVertex));
        transfer.submit(waits);
        CHECK(read_back(d, transfer, waits, *dests[round % 3u], data.size(), data));
    }
    const TransferQueue::Stats s = transfer.stats();
    CHECK(s.semaphores <= 3u);  // recycled, not re-created every round
    CHECK(s.semaphore_failures == 0u);
    transfer.destroy();
    CHECK(!transfer.live());
}

void test_retained_adaptive_chunks(Device& d) {
    std::printf("retained adaptive chunks and in-flight owner separation\n");
    TransferQueue transfer;
    TransferQueue::Config config;
    config.minimum_chunk_bytes = 64u << 10;
    config.staging_bytes = 512u << 10;
    config.batches = 2;
    std::string error;
    CHECK(transfer.create(*d.device, config, error));

    const std::size_t sizes[] = {48u << 10, 80u << 10, 144u << 10};
    std::vector<std::unique_ptr<plume::RenderBuffer>> dests;
    std::vector<std::vector<u8>> expected;
    for (u32 index = 0; index < 3u; ++index) {
        dests.push_back(d.device->createBuffer(plume::RenderBufferDesc::DefaultBuffer(sizes[index])));
        expected.push_back(pattern(sizes[index], 500u + index));
        CHECK(transfer.upload_buffer(*dests.back(), 0, expected.back().data(), expected.back().size(),
                                     TransferQueue::UploadOwner::GeometryVertex));
    }
    const TransferQueue::Stats grown = transfer.stats();
    CHECK(grown.early_submits == 0u);
    CHECK(grown.growths == 3u);
    CHECK(grown.retained_bytes == (448u << 10));
    CHECK(grown.peak_batch_bytes == (272u << 10));
    CHECK(grown.owner_uploads[static_cast<std::size_t>(TransferQueue::UploadOwner::GeometryVertex)] == 3u);
    CHECK(grown.owner_bytes[static_cast<std::size_t>(TransferQueue::UploadOwner::GeometryVertex)] ==
          (272u << 10));

    // Submit batch 0, then write batch 1 before the graphics queue consumes
    // batch 0's semaphore. Distinct fence owners must keep both byte sets live.
    std::vector<plume::RenderCommandSemaphore*> first_waits;
    transfer.submit(first_waits);
    auto second_dest = d.device->createBuffer(plume::RenderBufferDesc::DefaultBuffer(200u << 10));
    const std::vector<u8> second = pattern(200u << 10, 600u);
    CHECK(transfer.upload_buffer(*second_dest, 0, second.data(), second.size(),
                                 TransferQueue::UploadOwner::GeometryIndex));
    std::vector<plume::RenderCommandSemaphore*> second_waits;
    transfer.submit(second_waits);
    CHECK(first_waits.size() == 1u && second_waits.size() == 1u);

    CHECK(read_back(d, transfer, first_waits, *dests[0], sizes[0], expected[0]));
    for (u32 index = 1; index < 3u; ++index) {
        std::vector<plume::RenderCommandSemaphore*> no_wait;
        CHECK(read_back(d, transfer, no_wait, *dests[index], sizes[index], expected[index]));
    }
    CHECK(read_back(d, transfer, second_waits, *second_dest, second.size(), second));

    // Batch 0 is now retired. Reuse must rewind its retained chunks without a
    // growth and without retaining beyond the configured two-owner bound.
    const u64 growths_before = transfer.stats().growths;
    const std::vector<u8> reuse = pattern(32u << 10, 700u);
    CHECK(transfer.upload_buffer(*dests[0], 0, reuse.data(), reuse.size(),
                                 TransferQueue::UploadOwner::GeometryVertex));
    std::vector<plume::RenderCommandSemaphore*> reuse_waits;
    transfer.submit(reuse_waits);
    CHECK(read_back(d, transfer, reuse_waits, *dests[0], reuse.size(), reuse));
    const TransferQueue::Stats reused = transfer.stats();
    CHECK(reused.growths == growths_before);
    CHECK(reused.resets >= 1u);
    CHECK(reused.retained_bytes <= config.staging_bytes * config.batches);
    CHECK(reused.allocation_failures == 0u);
}

void test_fence_wait_contract(Device& d) {
    std::puts("bounded fence wait");
    auto fence = d.device->createCommandFence();
    // Never submitted: nothing to wait for on any backend's terms but D3D12's;
    // the renderer never waits a fence it did not submit, so only a signalled
    // submission is exercised here.
    auto src = d.device->createBuffer(plume::RenderBufferDesc::DefaultBuffer(64u << 20));
    auto dst = d.device->createBuffer(plume::RenderBufferDesc::DefaultBuffer(64u << 20));
    CHECK(src != nullptr && dst != nullptr);
    auto list = d.direct->createCommandList();
    list->begin();
    for (int i = 0; i < 8; ++i) {
        list->copyBufferRegion(plume::RenderBufferReference(dst.get()),
                               plume::RenderBufferReference(src.get()), 64u << 20);
    }
    list->end();
    const plume::RenderCommandList* lists[] = {list.get()};
    d.direct->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
    // A poll either finds it done or says TIMEOUT without consuming anything.
    plume::RenderFenceWait first = d.direct->waitForCommandFenceFor(fence.get(), 0u);
    CHECK(first == plume::RenderFenceWait::SIGNALED || first == plume::RenderFenceWait::TIMEOUT);
    if (first == plume::RenderFenceWait::TIMEOUT) {
        plume::RenderFenceWait result = plume::RenderFenceWait::TIMEOUT;
        for (int second = 0; second < 10 && result == plume::RenderFenceWait::TIMEOUT; ++second) {
            result = d.direct->waitForCommandFenceFor(fence.get(), 1000000000ull);
        }
        CHECK(result == plume::RenderFenceWait::SIGNALED);
    }
    // The same fence, signalled again, is waited again: once per submission.
    d.direct->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
    d.direct->waitForCommandFence(fence.get());
    // The device is still healthy afterwards.
    CHECK(d.device->createBuffer(plume::RenderBufferDesc::DefaultBuffer(4096)) != nullptr);
}

void test_fallible_factories(Device& d) {
    std::puts("fallible factories");
    // An extent no device accepts: the backend refuses it as invalid and the
    // factory must say so with null, not a wrapper around a null handle.
    // (Not an absurdly large buffer: on this machine's driver a 1 PiB request
    // removes the device instead of failing the call, measured 2026-09-28.)
    auto impossible = d.device->createTexture(plume::RenderTextureDesc::Texture2D(
        1u << 20, 1u << 20, 1, plume::RenderFormat::R8G8B8A8_UNORM));
    CHECK(impossible == nullptr);
    auto fine = d.device->createBuffer(plume::RenderBufferDesc::DefaultBuffer(4096));
    CHECK(fine != nullptr);
}

void test_memory_budget(Device& d) {
    std::puts("memory budget");
    plume::RenderMemoryBudget budget;
    CHECK(d.device->getMemoryBudget(budget));
    CHECK(budget.budgetBytes > 0u);
    auto held = d.device->createBuffer(plume::RenderBufferDesc::DefaultBuffer(32u << 20));
    plume::RenderMemoryBudget after;
    CHECK(d.device->getMemoryBudget(after));
    CHECK(after.usageBytes >= budget.usageBytes);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Device device;
    if (!open_device(device)) {
        std::printf("skip: no D3D12 device\n");
        return 77;
    }
    test_buffer_round_trip(device);
    test_texture_round_trip(device);
    test_bounds_and_early_submit(device);
    test_retained_adaptive_chunks(device);
    test_fence_wait_contract(device);
    test_fallible_factories(device);
    test_memory_budget(device);
    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
