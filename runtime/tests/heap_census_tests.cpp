// liveness-roots: reference-interpreter-only
// The OS heap census (gekko/heap_census.h, docs/MEMORY_ALLOCATORS.md) against the ORIGINAL words in the reference
// interpreter. The original OSInitAlloc 0x80372138, OSCreateHeap 0x803721A8, OSSetCurrentHeap 0x80372128,
// OSAllocFromHeap 0x80371FB0 and OSFreeToHeap 0x803720AC build and churn a real heap (random sizes, random frees,
// until it runs full and back); after every call the census of guest memory must agree with what the test knows and
// with the invariants the original keeps:
//   - allocated + free bytes = the heap's size; allocated cells = the live blocks; no walk error;
//   - every live pointer p has its cell at p - 0x20 in the allocated list, sized round32(request + 0x20) plus a
//     leftover under 0x40;
//   - the free list is address-sorted and coalesced (no two free cells touch);
//   - first fit (the reverse-engineered rule): each allocation takes the lowest-address free cell that fits; a failed
//     one happens only when no free cell fits.
// usage: heap_census_tests <sys/main.dol>

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/heap_census.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gekko;

namespace {

constexpr u32 kInitAlloc = 0x80372138u, kCreateHeap = 0x803721A8u, kSetCurrent = 0x80372128u;
constexpr u32 kAlloc = 0x80371FB0u, kFree = 0x803720ACu;
constexpr u32 kStack = 0x817E0000u, kR13 = 0x805EC500u, kR2 = 0x805FA780u, kSentinel = 0x80001200u;
constexpr u32 kArenaLo = 0x80700000u, kArenaHi = 0x80F00000u;  // 8 MiB

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
        // OSAllocFromHeap begins `mulli r0,r3,12`; OSInitAlloc stores the heap array at r13+0x5A60.
        if (bus.read32(kAlloc) != 0x1C03000Cu) throw std::runtime_error("unexpected OSAllocFromHeap words");
        cpu.reset();
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
    }
    u32 call(u32 pc, u32 r3, u32 r4 = 0u, u32 r5 = 0u) {
        cpu.gpr[1] = kStack;
        cpu.gpr[3] = r3;
        cpu.gpr[4] = r4;
        cpu.gpr[5] = r5;
        cpu.lr = kSentinel;
        cpu.pc = pc;
        unsigned long long steps = 0;
        while (cpu.pc != kSentinel && !cpu.stopped() && steps++ < 10'000'000ull) cpu.run_steps(1u);
        if (cpu.pc != kSentinel) throw std::runtime_error("call to " + hex(pc) + " did not return");
        return cpu.gpr[3];
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: heap_census_tests <sys/main.dol>\n");
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<u8> dol((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    try {
        Machine m(dol);
        const GuestRead32 read32 = [&](u32 a) { return m.bus.read32(a); };

        const u32 lo = m.call(kInitAlloc, kArenaLo, kArenaHi, 1u);
        const u32 start = (lo + 0x1Fu) & ~0x1Fu, end = kArenaHi & ~0x1Fu;
        const u32 heap = m.call(kCreateHeap, start, end);
        check(heap == 0u, "OSCreateHeap returns heap 0");
        m.call(kSetCurrent, heap);

        OsHeapsCensus c0 = census_os_heaps(read32);
        check(c0.error.empty() && c0.heaps == 1 && c0.current == 0, "globals after setup: " + c0.error);
        check(c0.arena_start == lo && c0.arena_end == kArenaHi, "arena bounds as OSInitAlloc set them");
        check(c0.census.size() == 1u && c0.census[0].size == static_cast<std::int32_t>(end - start),
              "heap size = end - start");
        check(c0.census[0].free_cells == 1u && c0.census[0].free_bytes == end - start, "one free cell over the heap");

        std::mt19937 rng(20261104u);
        std::map<u32, u32> live;  // pointer -> requested size
        const auto request = [&] {
            const u32 k = rng() % 100u;
            if (k < 60u) return 1u + rng() % 256u;
            if (k < 90u) return 257u + rng() % 4096u;
            return 4353u + rng() % (256u * 1024u);
        };
        unsigned long long allocs = 0, fulls = 0, frees = 0;
        for (int op = 0; op < 20000; ++op) {
            const OsHeapsCensus before = census_os_heaps(read32);
            const bool do_alloc = live.empty() || (rng() % 100u) < (op % 4000 < 2000 ? 70u : 35u);
            if (do_alloc) {
                const u32 size = request();
                const u32 need = (size + 0x20u + 0x1Fu) & ~0x1Fu;
                std::string err;
                const auto free_list =
                    os_heap_list(read32, m.bus.read32(before.heap_array + 4u), before.arena_start, before.arena_end, &err);
                u32 expected = 0u;
                for (const auto& [cell, csize] : free_list)
                    if (csize >= need) {
                        expected = cell;
                        break;
                    }
                const u32 p = m.call(kAlloc, heap, size);
                if (p == 0u) {
                    ++fulls;
                    check(expected == 0u, "allocation of " + hex(size) + " failed with a fitting cell at " + hex(expected));
                } else {
                    ++allocs;
                    check(p == expected + 0x20u, "first fit: " + hex(p) + " for " + hex(size) + ", expected " +
                                                     hex(expected + 0x20u));
                    live[p] = size;
                }
            } else {
                auto it = live.begin();
                std::advance(it, static_cast<long>(rng() % live.size()));
                m.call(kFree, heap, it->first);
                live.erase(it);
                ++frees;
            }
            const OsHeapsCensus c = census_os_heaps(read32);
            const OsHeapCensus& h = c.census.at(0);
            check(c.error.empty() && h.error.empty(), "walk error at op " + std::to_string(op) + ": " + c.error + h.error);
            check(h.allocated_bytes + h.free_bytes == static_cast<u32>(h.size),
                  "bytes conserved at op " + std::to_string(op));
            check(h.allocated_cells == live.size(), "allocated cells = live blocks at op " + std::to_string(op));
            check(h.free_sorted && h.free_coalesced, "free list sorted and coalesced at op " + std::to_string(op));
            if (op % 500 == 0 || op == 19999) {
                std::string err;
                std::map<u32, u32> used;
                for (const auto& [cell, size] :
                     os_heap_list(read32, m.bus.read32(c.heap_array + 8u), c.arena_start, c.arena_end, &err))
                    used[cell] = size;
                for (const auto& [p, size] : live) {
                    const u32 need = (size + 0x20u + 0x1Fu) & ~0x1Fu;
                    const auto found = used.find(p - 0x20u);
                    check(found != used.end() && found->second >= need && found->second < need + 0x40u,
                          "live block " + hex(p) + " has its cell, sized for " + hex(size));
                }
            }
        }
        std::printf("heap_census_tests: %llu allocations, %llu refused (heap full), %llu frees, %llu checks, %d failures\n",
                    allocs, fulls, frees, g_checks, g_failures);
        if (fulls == 0u) check(false, "the churn never filled the heap (the full path was not exercised)");
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
    return g_failures == 0 ? 0 : 1;
}
