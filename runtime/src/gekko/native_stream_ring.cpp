// CRI's audio stream ring without ARAM. Contract and the original code's evidence: include/gekko/native_stream_ring.h
// and docs/ARAM.md.

#include "gekko/native_stream_ring.h"

#include <atomic>
#include <string>

#include "gekko/cpu.h"

namespace gekko {

namespace {

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

// The native ADX output's transfers (docs/CRI_SERVERS.md 4c; until 2026-11-03 the lowered ARQPostRequest sites
// 0x803CF0D0 / 0x803CF21C, whose completions ran at the DSP DMA interrupt): the chunk into the ring.
bool NativeStreamRing::copy(u32 source, u32 destination, u32 bytes, std::string& why) {
    if (!arena_.copy_in(destination, source, bytes, why)) return false;
    stats_.committed = arena_.committed();
    ++stats_.posts;
    stats_.post_bytes += bytes;
    return true;
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

}  // namespace scaffold

}  // namespace gekko
