// CRI's audio stream ring without ARAM. Contract and the original code's evidence: include/gekko/native_stream_ring.h
// and docs/ARAM.md.

#include "gekko/native_stream_ring.h"

#include <atomic>
#include <string>

#include "gekko/cpu.h"
#include "gekko/native_audio.h"

namespace gekko {

namespace {

// ARQPostRequest's request record (0x80384500 fills it before queueing).
constexpr u32 kRequestNext = 0x0u;
constexpr u32 kRequestOwner = 0x4u;
constexpr u32 kRequestType = 0x8u;
constexpr u32 kRequestSource = 0x10u;
constexpr u32 kRequestDestination = 0x14u;
constexpr u32 kRequestLength = 0x18u;
constexpr u32 kRequestCallback = 0x1Cu;

std::atomic<NativeStreamRing*> g_installed{nullptr};

NativeStreamRing* installed(Cpu& c, const char* entry) {
    NativeStreamRing* ring = g_installed.load(std::memory_order_acquire);
    if (ring == nullptr)
        c.request_stop(StopReason::ExecutionFault,
                       std::string("lowered stream-ring site (scaffold::") + entry + ") ran without the ring installed");
    return ring;
}

}  // namespace

void NativeStreamRing::install() {
    g_installed.store(this, std::memory_order_release);
}

void NativeStreamRing::stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "stream ring: " + why);
}

// 0x803D06E0 in the ring setup: ARAlloc(bytes).
u32 NativeStreamRing::reserve(Cpu& c, u32 bytes) {
    std::string why;
    if (!arena_.reserve(bytes, why) || !arena_.commit_reserve(why)) {
        stop(c, why);
        return 0u;
    }
    stats_.reserved = arena_.reserved();
    stats_.committed = arena_.committed();
    ++stats_.reserves;
    return kSoundBase;
}

// 0x803D0658 in the ring finalize: ARFree(&size) - the block's base back, its size written where r3 points (the
// finalize compares it with the size it asked for).
u32 NativeStreamRing::release(Cpu& c, u32 size_out) {
    const u32 size = arena_.requested();
    const u32 base = arena_.sound_base();
    std::string why;
    if (!arena_.release(why)) {
        stop(c, why);
        return 0u;
    }
    if (size_out != 0u)
        c.mem_write32(size_out, size);
    ++stats_.releases;
    return base;
}

// 0x803CF0D0 / 0x803CF21C: ARQPostRequest(request, owner, type, priority, source, destination, bytes, callback).
// The record as ARQPostRequest leaves it (the callbacks read it), the chunk in the ring, the completion handed to the
// DSP DMA interrupt.
void NativeStreamRing::post(Cpu& c, u32 request, u32 owner, u32 type, u32 source, u32 destination, u32 bytes,
                            u32 callback) {
    if (type != 0u) {
        stop(c, "a transfer out of the ring (type " + std::to_string(type) + "; the stream server only feeds it)");
        return;
    }
    // Both of the stream server's transfers name their completion (0x803CF32C, 0x803CF260); ARQ's stand-in for a
    // null one (a bare blr) is not a path of the game.
    if (callback == 0u) {
        stop(c, "a transfer without a completion callback");
        return;
    }
    const u32 completion = callback;
    c.mem_write32(request + kRequestNext, 0u);
    c.mem_write32(request + kRequestOwner, owner);
    c.mem_write32(request + kRequestType, type);
    c.mem_write32(request + kRequestSource, source);
    c.mem_write32(request + kRequestDestination, destination);
    c.mem_write32(request + kRequestLength, bytes);
    c.mem_write32(request + kRequestCallback, completion);
    std::string why;
    if (!arena_.copy_in(destination, source, bytes, why)) {
        stop(c, why);
        return;
    }
    stats_.committed = arena_.committed();
    ++stats_.posts;
    stats_.post_bytes += bytes;
    audio_.complete_transfer(completion, request);
}

// ---- the lowered call sites (extract/lift-sites-native.txt; declared by the generator in recomp_table.cpp) --------

namespace scaffold {

u32 stream_ring_reserve(Cpu& c, u32 bytes) {
    NativeStreamRing* ring = installed(c, "stream_ring_reserve");
    return ring != nullptr ? ring->reserve(c, bytes) : 0u;
}

u32 stream_ring_release(Cpu& c, u32 size_out) {
    NativeStreamRing* ring = installed(c, "stream_ring_release");
    return ring != nullptr ? ring->release(c, size_out) : 0u;
}

u32 stream_ring_post(Cpu& c, u32 request, u32 owner, u32 type, u32 priority, u32 source, u32 destination, u32 bytes,
                     u32 callback) {
    (void)priority;  // ARQ's queue choice; nothing queues here
    NativeStreamRing* ring = installed(c, "stream_ring_post");
    if (ring != nullptr)
        ring->post(c, request, owner, type, source, destination, bytes, callback);
    return 0u;
}

}  // namespace scaffold

}  // namespace gekko
