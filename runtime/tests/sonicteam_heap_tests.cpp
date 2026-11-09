// liveness-roots: reference-interpreter-only
// Sonicteam::System::Heap in C++20 (gekko/sonicteam_heap.h, docs/MEMORY_ALLOCATORS.md) against the ORIGINAL words in
// the reference interpreter. Two machines with the same DOL and the same addresses: on one the original constructor
// 0x8040C040 and methods vf0..vf7 run word for word (vf2 / vf6 reach vf0 / vf4 through the vtable 0x8056AF2C, memset
// 0x8000540C and memcpy 0x800054F4 run as the game's code); on the other gekko::sonicteam::Heap drives the same memory.
// A seeded random program of Alloc / AllocAligned (alignments 8..256; see min_align) / Calloc / Free / Realloc (NULL and live blocks)
// / LargestFree / TotalFree runs on both, the heap running full and back; every allocated block is then filled with a
// pattern on both (so Realloc's copy is checked). After every call: the return value, the 8-byte object and every byte
// of the heap buffer must be equal.
// usage: sonicteam_heap_tests <sys/main.dol>

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/sonicteam_heap.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gekko;

namespace {

constexpr u32 kCtor = 0x8040C040u, kAlloc = 0x8040BFA8u, kAllocAligned = 0x8040BEB0u, kCalloc = 0x8040BE60u;
constexpr u32 kFree = 0x8040BDB8u, kLargest = 0x8040BD88u, kRealloc = 0x8040BCB8u, kTotal = 0x8040BC90u;
constexpr u32 kStack = 0x817E0000u, kR13 = 0x805EC500u, kR2 = 0x805FA780u, kSentinel = 0x80001200u;
constexpr u32 kSelf = 0x80700000u, kBuffer = 0x80710000u, kBufferSize = 0x40000u;  // 256 KiB

int g_failures = 0;
unsigned long long g_checks = 0;
void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok && ++g_failures <= 40) std::printf("FAIL: %s\n", what.c_str());
}
std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08X", v);
    return b;
}

struct Machine {
    GuestMemory bus;
    Cpu cpu{bus};
    explicit Machine(const std::vector<u8>& dol) {
        const auto word = [&](std::size_t o) {
            return u32{dol[o]} << 24 | u32{dol[o + 1]} << 16 | u32{dol[o + 2]} << 8 | u32{dol[o + 3]};
        };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (size == 0u) continue;
            if (std::uint64_t(offset) + size > dol.size()) throw std::runtime_error("invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        if (bus.read32(sonicteam::kHeapVtable + 8u) != kAlloc || bus.read32(sonicteam::kHeapVtable + 0x18u) != kFree)
            throw std::runtime_error("unexpected Sonicteam::System::Heap vtable");
        cpu.reset();
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
        for (u32 i = 0; i < kBufferSize; i += 4u) bus.write32(kBuffer + i, 0xEEEEEEEEu);
        bus.write32(kSelf, 0xEEEEEEEEu);
        bus.write32(kSelf + 4u, 0xEEEEEEEEu);
    }
    u32 call(u32 pc, u32 r3, u32 r4 = 0u, u32 r5 = 0u) {
        cpu.gpr[1] = kStack;
        cpu.gpr[3] = r3;
        cpu.gpr[4] = r4;
        cpu.gpr[5] = r5;
        cpu.lr = kSentinel;
        cpu.pc = pc;
        unsigned long long steps = 0;
        while (cpu.pc != kSentinel && !cpu.stopped() && steps++ < 20'000'000ull) cpu.run_steps(1u);
        if (cpu.pc != kSentinel) throw std::runtime_error("call to " + hex(pc) + " did not return");
        return cpu.gpr[3];
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: sonicteam_heap_tests <sys/main.dol>\n");
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<u8> dol((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    try {
        Machine original(dol), native(dol);
        sonicteam::GuestAccess access;
        access.read32 = [&](u32 a) { return native.bus.read32(a); };
        access.write32 = [&](u32 a, u32 v) { native.bus.write32(a, v); };
        access.copy = [&](u32 d, u32 s, u32 n) {  // the game's memcpy 0x800054F4 is overlap-safe (memmove)
            if (d > s && d < s + n) {
                for (u32 i = n; i-- > 0u;) native.bus.write8(d + i, native.bus.read8(s + i));
            } else {
                for (u32 i = 0; i < n; ++i) native.bus.write8(d + i, native.bus.read8(s + i));
            }
        };
        access.fill = [&](u32 d, u8 v, u32 n) {
            for (u32 i = 0; i < n; ++i) native.bus.write8(d + i, v);
        };
        sonicteam::Heap heap(access, kSelf);

        const auto same_memory = [&](const std::string& after) {
            bool ok = original.bus.read32(kSelf) == native.bus.read32(kSelf) &&
                      original.bus.read32(kSelf + 4u) == native.bus.read32(kSelf + 4u);
            u32 first = 0u;
            for (u32 i = 0; ok && i < kBufferSize; i += 4u)
                if (original.bus.read32(kBuffer + i) != native.bus.read32(kBuffer + i)) {
                    ok = false;
                    first = kBuffer + i;
                }
            check(ok, "memory differs after " + after + (first ? " at " + hex(first) : std::string(" (object)")));
            if (!ok && first != 0u) {  // the words around the first difference, original / native
                u32 last = first;
                for (u32 at = first; at < kBuffer + kBufferSize; at += 4u)
                    if (original.bus.read32(at) != native.bus.read32(at)) last = at;
                std::printf("  differing range %s..%s\n", hex(first).c_str(), hex(last + 3u).c_str());
                for (u32 at = first - 16u; at < first + 32u; at += 4u)
                    std::printf("  %s  %s  %s\n", hex(at).c_str(), hex(original.bus.read32(at)).c_str(),
                                hex(native.bus.read32(at)).c_str());
            }
        };

        original.call(kCtor, kSelf, kBuffer, kBufferSize);
        heap.construct(kBuffer, kBufferSize);
        same_memory("the constructor");

        std::mt19937 rng(20261104u);
        std::map<u32, u32> live;  // pointer -> size asked
        const auto size_of = [&] {
            const u32 k = rng() % 100u;
            if (k < 5u) return 0u;
            if (k < 65u) return 1u + rng() % 128u;
            if (k < 92u) return 129u + rng() % 2048u;
            return 2177u + rng() % 16384u;
        };
        // The smallest AllocAligned alignment (then x2 .. x32); default 8. Measured 2026-11-04: with 4 the ORIGINAL
        // corrupts its own heap - need = 4 + a multiple of 8 leaves blocks off 8-byte alignment, later padding leaves
        // free fragments smaller than a header that overlap the next block, and a Free writes through them - until
        // its free list is a cycle and LargestFree 0x8040BD88 never returns (docs/MEMORY_ALLOCATORS.md). The
        // allocator's contract is an alignment that is a multiple of 8; pass 4 to reproduce the hazard.
        const u32 min_align = argc > 2 ? static_cast<u32>(std::strtoul(argv[2], nullptr, 0)) : 8u;
        unsigned long long short_blocks = 0;
        const auto fill = [&](u32 p, u32 size, u32 seed) {
            // What the block really holds: its header-inclusive size (at p - 4) less the 8-byte header. A block
            // smaller than the request is the allocator's doing (reported; the fill keeps inside the block).
            const u32 holds = original.bus.read32(p - 4u) - 8u;
            if (holds < size) {
                if (++short_blocks <= 5u)
                    std::printf("  NOTE: block %s holds %s bytes for a request of %s\n", hex(p).c_str(),
                                hex(holds).c_str(), hex(size).c_str());
                size = holds;
            }
            for (u32 i = 0; i < size; ++i) {
                const u8 b = static_cast<u8>(seed * 131u + i * 7u);
                original.bus.write8(p + i, b);
                native.bus.write8(p + i, b);
            }
        };
        unsigned long long counts[8] = {};
        unsigned long long refused = 0;
        for (int op = 0; op < 30000; ++op) {
            const bool grow = (op % 6000) < 3000;
            const u32 k = rng() % 100u;
            std::string what;
            u32 a = 0u, b = 0u;
            if (live.empty() || k < (grow ? 40u : 15u)) {
                const u32 size = size_of();
                a = original.call(kAlloc, kSelf, size);
                b = heap.alloc(size);
                what = "Alloc(" + hex(size) + ")";
                ++counts[0];
                if (a && a == b) { live[a] = size; fill(a, size, static_cast<u32>(op)); }
            } else if (k < (grow ? 55u : 25u)) {
                const u32 align = min_align << (rng() % 6u), size = size_of();
                a = original.call(kAllocAligned, kSelf, align, size);
                b = heap.alloc_aligned(align, size);
                what = "AllocAligned(" + hex(align) + ", " + hex(size) + ")";
                ++counts[1];
                if (a && a == b) {
                    check(a % align == 0u, what + " returned a misaligned " + hex(a));
                    live[a] = size;
                    fill(a, size, static_cast<u32>(op));
                }
            } else if (k < (grow ? 62u : 32u)) {
                const u32 size = size_of();
                a = original.call(kCalloc, kSelf, size);
                b = heap.calloc(size);
                what = "Calloc(" + hex(size) + ")";
                ++counts[2];
                if (a && a == b) live[a] = size;
            } else if (k < 80u) {
                auto it = live.begin();
                std::advance(it, static_cast<long>(rng() % live.size()));
                a = original.call(kFree, kSelf, it->first);
                heap.free(it->first);
                b = a;  // void
                what = "Free(" + hex(it->first) + ")";
                live.erase(it);
                ++counts[4];
            } else if (k < 92u) {
                const bool null = rng() % 8u == 0u;
                u32 p = 0u;
                if (!null) {
                    auto it = live.begin();
                    std::advance(it, static_cast<long>(rng() % live.size()));
                    p = it->first;
                }
                // Now and then exactly the stored size, the original's identity case.
                const u32 size = (!null && rng() % 10u == 0u) ? native.bus.read32(p - 4u) : size_of();
                a = original.call(kRealloc, kSelf, p, size);
                b = heap.realloc(p, size);
                what = "Realloc(" + hex(p) + ", " + hex(size) + ")";
                ++counts[6];
                if (a && a == b) {
                    if (p != 0u && a != p) live.erase(p);
                    // The original's identity case returns p when the block's header-inclusive size equals the
                    // request, though the block holds only size - 8 bytes: a caller writing `size` bytes runs into
                    // the next block's header. The test keeps to what the block really holds.
                    live[a] = (a == p) ? std::min(size, native.bus.read32(p - 4u) - 8u) : size;
                }
            } else if (k < 96u) {
                a = original.call(kLargest, kSelf);
                b = heap.largest_free();
                what = "LargestFree";
                ++counts[5];
            } else {
                a = original.call(kTotal, kSelf);
                b = heap.total_free();
                what = "TotalFree";
                ++counts[7];
            }
            if (a == 0u && what.find("Alloc") != std::string::npos) ++refused;
            check(a == b, what + ": original " + hex(a) + ", native " + hex(b) + " at op " + std::to_string(op));
            const int failures_before = g_failures;
            same_memory(what + " at op " + std::to_string(op));
            if (a != b || g_failures != failures_before) break;  // the two heaps differ from here on
        }
        same_memory("the last call");
        std::printf("sonicteam_heap_tests: Alloc %llu, AllocAligned %llu, Calloc %llu, Free %llu, Realloc %llu, "
                    "LargestFree %llu, TotalFree %llu; %llu allocations refused (full); %llu checks, %d failures\n",
                    counts[0], counts[1], counts[2], counts[4], counts[6], counts[5], counts[7], refused, g_checks,
                    g_failures);
        if (refused == 0u) check(false, "the program never filled the heap");
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
    return g_failures == 0 ? 0 : 1;
}
