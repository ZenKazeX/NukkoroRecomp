// The motion clips without ARAM. Contract and the original code's evidence: include/gekko/native_motion_store.h and
// docs/ARAM.md.

#include "gekko/native_motion_store.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/recomp.h"

namespace gekko {

namespace {

// ---- the entry points the generator routes here (extract/native-owners.txt)
constexpr u32 kPoolCreate = 0x804225B0u;
constexpr u32 kPoolDestroy = 0x80422550u;
constexpr u32 kUpload = 0x804222C0u;
constexpr u32 kFetch = 0x80422234u;

const NativeMotionStore::Address kAddresses[] = {
    {kPoolCreate, "motion store: pool create (0x804225B0)"},
    {kPoolDestroy, "motion store: pool destroy (0x80422550)"},
    {kUpload, "motion store: clip upload (0x804222C0)"},
    {kFetch, "motion store: clip fetch (0x80422234)"},
};

// The guest helpers the pool's create and destroy call, as the original does.
constexpr u32 kHeapAlloc = 0x80412184u;     // (r3 1, r4 bytes) -> the motion system's heap
constexpr u32 kPoolNodeFree = 0x80422614u;  // (r3 node, r4 1): unlinks a pool node and frees it

constexpr u32 kSdaPoolBase = 0x5F98u;  // the pool's base (the original's ARAlloc result, kept across stages)
constexpr u32 kSdaPoolHead = 0x5F9Cu;  // the pool's first node: {prev, next (itself), base, size}

}  // namespace

const NativeMotionStore::Address* NativeMotionStore::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

void NativeMotionStore::install() {
    if (const char* verify = std::getenv("SHADOW_MOTION_VERIFY"))
        verify_ = verify[0] == '1';
    Cpu& cpu = cpu_;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, name);
    };
    own(kPoolCreate, kAddresses[0].name, [this](Cpu& c) { pool_create(c); });
    own(kPoolDestroy, kAddresses[1].name, [this](Cpu& c) { pool_destroy(c); });
    own(kUpload, kAddresses[2].name, [this](Cpu& c) { upload(c); });
    own(kFetch, kAddresses[3].name, [this](Cpu& c) { fetch(c); });
}

void NativeMotionStore::stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "motion store: " + why);
}

// 0x804225B0: once, the pool's base (the original ARAllocs 6 MiB here); each stage, its first node from the heap -
// the same allocation, so the heap is the original's.
void NativeMotionStore::pool_create(Cpu& c) {
    if (!ensure_arena(c))
        return;
    if (c.mem_read32(c.gpr[13] + kSdaPoolBase) == 0u)
        c.mem_write32(c.gpr[13] + kSdaPoolBase, kPoolBase);
    if (!call_guest(c, kHeapAlloc, 1u, 0x10u))
        return;
    const u32 head = c.gpr[3];
    if (head != 0u) {
        c.mem_write32(head, 0u);
        c.mem_write32(head + 4u, head);
        c.mem_write32(head + 8u, c.mem_read32(c.gpr[13] + kSdaPoolBase));
        c.mem_write32(head + 0xCu, 0u);
    }
    c.mem_write32(c.gpr[13] + kSdaPoolHead, head);
    ++stats_.pools;
}

// 0x80422550: the pool's nodes freed as the original frees them; no ARFree (nothing was taken from ARAM).
void NativeMotionStore::pool_destroy(Cpu& c) {
    u32 head = c.mem_read32(c.gpr[13] + kSdaPoolHead);
    if (head == 0u)
        return;
    for (u32 guard = 0; head != 0u && c.mem_read32(head + 4u) != 0u; ++guard) {
        if (guard >= 0x100000u) {
            stop(c, "the pool's node list does not end");
            return;
        }
        if (!call_guest(c, kPoolNodeFree, c.mem_read32(head + 4u), 1u))
            return;
        head = c.mem_read32(c.gpr[13] + kSdaPoolHead);
    }
    call_guest(c, kPoolNodeFree, head, 1u);
}

// 0x804222C0 (r3 pool address, r4 the game's buffer, r5 bytes): the clip span into the pool.
void NativeMotionStore::upload(Cpu& c) {
    const u32 to = pool_address(c.gpr[3]), from = c.gpr[4], bytes = c.gpr[5];
    if (!ensure_committed(c, to + bytes))
        return;
    u8* destination = memory_.span(GcAddr(to), bytes);
    const u8* source = memory_.span(GcAddr(from), bytes);
    if (destination == nullptr || source == nullptr) {
        stop(c, "an upload outside mapped memory");
        return;
    }
    std::memcpy(destination, source, bytes);
    take_interrupts(c);
    ++stats_.uploads;
    stats_.upload_bytes += bytes;
    if (verify_) {  // the same hash as the ARAM census's "[motion-span]" line for the original's upload
        u64 hash = 0xCBF29CE484222325ull;
        for (u32 i = 0; i < bytes; ++i)
            hash = (hash ^ source[i]) * 0x100000001B3ull;
        std::fprintf(stderr, "[motion-span] bytes %u hash %016llX\n", bytes, static_cast<unsigned long long>(hash));
    }
}

// 0x80422234 (r3 the node's buffer, r4 pool address, r5 bytes): a clip part out of the pool.
void NativeMotionStore::fetch(Cpu& c) {
    const u32 to = c.gpr[3], from = pool_address(c.gpr[4]), bytes = c.gpr[5];
    u8* destination = memory_.span(GcAddr(to), bytes);
    const u8* source = memory_.span(GcAddr(from), bytes);
    if (destination == nullptr || source == nullptr) {
        stop(c, "a fetch outside mapped memory");
        return;
    }
    std::memcpy(destination, source, bytes);
    ++stats_.fetches;
    stats_.fetch_bytes += bytes;
    take_interrupts(c);
}

// The original transfers end in a loop that waits for the ARQ's completion; the loop's back edge is where the CPU
// takes the interrupts that are due (the transfer's own, and any audio or video one pending then), so callbacks run
// at that point of the motion code. With the copy done at once there is nothing to wait for, but that point stays:
// a deliverable interrupt is taken here, through the dispatcher, as at the loop's back edge.
void NativeMotionStore::take_interrupts(Cpu& c) {
    if (c.needs_dispatch()) {
        ++stats_.interrupt_points;
        c.leave_to_dispatcher(c.lr);
    }
}

bool NativeMotionStore::adopt_pool(u32 base, u32 bytes, const u8* image, std::string& why) {
    const u32 to = pool_address(base);
    if (base >= kAramEnd || bytes > kAramEnd - base) {
        why = "a pool outside ARAM's addresses";
        return false;
    }
    if (memory_.region_at(kArenaBase) == nullptr &&
        !memory_.map(kArenaBase, kArenaReserve, GuestMemory::kGranule, GuestMemory::RegionKind::Arena,
                     "native motion pool")) {
        why = "the arena could not be mapped";
        return false;
    }
    const u32 needed = (to + bytes - kArenaBase + GuestMemory::kGranule - 1u) & ~(GuestMemory::kGranule - 1u);
    const GuestMemory::Region* region = memory_.region_at(kArenaBase);
    if (region->committed < needed && !memory_.commit(kArenaBase, needed)) {
        why = "the arena could not commit " + std::to_string(needed) + " bytes";
        return false;
    }
    std::memcpy(region->host + (to - kArenaBase), image, bytes);
    stats_.committed = std::max(stats_.committed, needed);
    return true;
}

bool NativeMotionStore::ensure_arena(Cpu& c) {
    if (memory_.region_at(kArenaBase) == nullptr &&
        !memory_.map(kArenaBase, kArenaReserve, GuestMemory::kGranule, GuestMemory::RegionKind::Arena,
                     "native motion pool")) {
        stop(c, "the arena could not be mapped");
        return false;
    }
    return true;
}

bool NativeMotionStore::ensure_committed(Cpu& c, u32 end) {
    const GuestMemory::Region* region = memory_.region_at(kArenaBase);
    if (region == nullptr || end < kArenaBase || end > kArenaBase + kArenaReserve) {
        stop(c, "an upload outside the arena");
        return false;
    }
    const u32 needed = (end - kArenaBase + GuestMemory::kGranule - 1u) & ~(GuestMemory::kGranule - 1u);
    if (region->committed < needed && !memory_.commit(kArenaBase, needed)) {
        stop(c, "the arena could not commit " + std::to_string(needed) + " bytes");
        return false;
    }
    stats_.committed = std::max(stats_.committed, needed);
    return true;
}

// A guest call (r3, r4) returning here; the owner's own caller is left as it was.
bool NativeMotionStore::call_guest(Cpu& c, u32 function, u32 r3, u32 r4) {
    const u32 link = c.lr;
    c.gpr[3] = r3;
    c.gpr[4] = r4;
    c.gpr[12] = function;
    c.ctr = function;
    recomp::call_indirect(c, function, link);
    if (c.exiting())
        c.finish_transfers();
    if (c.exiting() || c.pc != link) {
        stop(c, "a guest call did not return");
        return false;
    }
    c.lr = link;
    return true;
}

}  // namespace gekko
