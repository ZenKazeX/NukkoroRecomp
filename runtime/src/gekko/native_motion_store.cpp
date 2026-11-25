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
    if (!ensure_storage(c))
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
    if (upload_span(c, c.gpr[3], c.gpr[4], c.gpr[5]))
        take_interrupts(c);
}

bool NativeMotionStore::upload_span(Cpu& c, u32 pool_to, u32 from, u32 bytes) {
    const u32 to = pool_address(pool_to);
    if (bytes == 0u) {  // nothing to copy (a zero-sized span resolves to no host memory: not a fault)
        ++stats_.uploads;
        return true;
    }
    if (to < kPoolBase || bytes > kPoolReserve || to - kPoolBase > kPoolReserve - bytes) {
        stop(c, "an upload outside the pool");
        return false;
    }
    if (!storage_.present() || !commit_through(to + bytes)) {
        stop(c, "the pool could not commit " + std::to_string(to - kPoolBase + bytes) + " bytes");
        return false;
    }
    u8* destination = pool_bytes(to, bytes);
    const u8* source = memory_.span(GcAddr(from), bytes);  // the game's buffer: a guest read (traced as one)
    if (destination == nullptr || source == nullptr) {
        stop(c, "an upload from outside mapped memory");
        return false;
    }
    // The pool is native storage: the verifier's journal cannot put it back, so a journalled run is told it had an
    // effect outside guest memory (GuestMemory::journal_note_host_effect).
    memory_.journal_note_host_effect();
    std::memcpy(destination, source, bytes);
    ++stats_.uploads;
    stats_.upload_bytes += bytes;
    if (verify_) {  // the same hash as the ARAM census's "[motion-span]" line for the original's upload
        u64 hash = 0xCBF29CE484222325ull;
        for (u32 i = 0; i < bytes; ++i)
            hash = (hash ^ source[i]) * 0x100000001B3ull;
        std::fprintf(stderr, "[motion-span] bytes %u hash %016llX\n", bytes, static_cast<unsigned long long>(hash));
    }
    return true;
}

// 0x80422234 (r3 the node's buffer, r4 pool address, r5 bytes): a clip part out of the pool.
void NativeMotionStore::fetch(Cpu& c) {
    if (fetch_span(c, c.gpr[3], c.gpr[4], c.gpr[5]))
        take_interrupts(c);
}

bool NativeMotionStore::fetch_span(Cpu& c, u32 to, u32 pool_from, u32 bytes) {
    const u32 from = pool_address(pool_from);
    if (bytes == 0u) {  // an empty clip part
        ++stats_.fetches;
        return true;
    }
    const u8* source = pool_bytes(from, bytes);
    if (source == nullptr) {
        stop(c, "a fetch outside the pool's committed bytes");
        return false;
    }
    // Into the game's memory: a guest-visible write, journalled (rolled back) and traced like a CPU store when the
    // verifier's journal or an access trace is on.
    if (memory_.journaling() || memory_.access_traced()) {
        if (!memory_.write(GcAddr(to), source, bytes)) {
            stop(c, "a fetch into unmapped memory");
            return false;
        }
    } else {
        u8* destination = memory_.span(GcAddr(to), bytes);
        if (destination == nullptr) {
            stop(c, "a fetch into unmapped memory");
            return false;
        }
        std::memcpy(destination, source, bytes);
    }
    ++stats_.fetches;
    stats_.fetch_bytes += bytes;
    return true;
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
    if (!commit_through(to + bytes)) {
        why = "the pool could not commit " + std::to_string(to - kPoolBase + bytes) + " bytes";
        return false;
    }
    std::memcpy(pool_bytes(to, bytes), image, bytes);
    return true;
}

const u8* NativeMotionStore::pool_bytes(u32 address, u32 bytes) const noexcept {
    const u32 at = pool_address(address);
    return at >= kPoolBase ? storage_.at(at - kPoolBase, bytes) : nullptr;
}

u8* NativeMotionStore::pool_bytes(u32 address, u32 bytes) noexcept {
    const u32 at = pool_address(address);
    return at >= kPoolBase ? storage_.at(at - kPoolBase, bytes) : nullptr;
}

// The storage as the region it replaces was: reserved at kPoolReserve with its first granule committed (the pool's
// create), then committed further as uploads reach.
bool NativeMotionStore::commit_through(u32 end) noexcept {
    if (end < kPoolBase || end - kPoolBase > kPoolReserve) return false;
    if (!storage_.present() && (!storage_.reserve(kPoolReserve) || !storage_.commit_through(NativeArenaStorage::kGranule)))
        return false;
    if (!storage_.commit_through(end - kPoolBase)) return false;
    stats_.committed = std::max(stats_.committed, storage_.committed());
    return true;
}

bool NativeMotionStore::ensure_storage(Cpu& c) {
    if (!storage_.present() && !commit_through(kPoolBase)) {
        stop(c, "the pool's storage could not be reserved");
        return false;
    }
    return true;
}

bool NativeMotionStore::image(NativeArenaImage& out) const noexcept {
    if (!storage_.present()) return false;
    out = NativeArenaImage{"native motion pool", kRecordLabel, storage_.size(), storage_.committed(), storage_.data()};
    return true;
}

bool NativeMotionStore::restore_image(u32 size, u32 committed, const u8* bytes, std::string& why) {
    if (size != kPoolReserve || !storage_.restore(size, committed, bytes)) {
        why = "native motion pool: the record's bytes could not be restored";
        return false;
    }
    stats_.committed = std::max(stats_.committed, storage_.committed());
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
