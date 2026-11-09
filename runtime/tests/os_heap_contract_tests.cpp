// liveness-roots: reference-interpreter-only
// Original-DOL contract for the owned OS heap routines (OSInitAlloc, OSCreateHeap, OSSetCurrentHeap, OSAllocFromHeap,
// OSFreeToHeap; gekko/native_os_heap.h).
// No generated reference body: every SDK instruction comes from the pinned DOL. Two machines are loaded with the same
// DOL image and given the same randomised register file and the same call; machine A runs the original instructions in
// the reference interpreter, machine B the native owner. After EVERY call the result (r3; OSFreeToHeap has none),
// the registers the calling convention preserves (r1, r2, r13-r31, LR) and every byte of the heap, the heap
// descriptors and the small-data cells (the arena words) must be equal. Scratch registers, CR, CTR and the dead stack
// frame the original leaves are not compared: no caller can rely on them. The calls are random sequences over several heap shapes (one big heap like the game's, several small heaps,
// exhausted descriptors, unaligned bounds, refused allocations, exact fits and leftovers on the 0x40 boundary) plus a
// few single calls with wrapped sizes. The census of the native side's memory checks the lists stay sane, so the
// sequences reach real states and not only equal garbage.
// usage: os_heap_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/heap_census.h"
#include "gekko/native_os_heap.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
using namespace gekko::os_heap;

constexpr u32 kStack = 0x817E0000u, kR13 = 0x805EC500u, kR2 = 0x805FA780u, kSentinel = 0x80001200u;
constexpr u32 kArenaLo = 0x80700000u;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
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
            require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        require(bus.read32(kAllocFromHeap) == 0x1C03000Cu, "unexpected OSAllocFromHeap words");
        require(bus.read32(kFreeToHeap) == 0x7C0802A6u, "unexpected OSFreeToHeap words");
        cpu.reset();
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
    }
    u32 read32(u32 a) { return bus.read32(a); }
};

struct Call {
    u32 entry;
    u32 r3, r4, r5;
};

std::vector<u8> g_dol;

// The whole machine state the contract compares.
struct Snapshot {
    u32 gpr[32];
    u32 cr, ctr, lr, xer;
};

Snapshot snapshot(const Cpu& c) {
    Snapshot s{};
    for (int i = 0; i < 32; ++i) s.gpr[i] = c.gpr[i];
    s.cr = c.cr;
    s.ctr = c.ctr;
    s.lr = c.lr;
    s.xer = c.xer;
    return s;
}

struct Pair {
    Machine a, b;  // a: the original words, b: the native owner
    std::mt19937 rng;
    u32 heap_lo, heap_hi;  // the compared memory (the arena)
    unsigned long long calls = 0, checks = 0;
    explicit Pair(unsigned seed, u32 lo, u32 hi) : a(g_dol), b(g_dol), rng(seed), heap_lo(lo), heap_hi(hi) {}

    // Same scrambled register file on both machines, then the call's arguments.
    void arm(Machine& m, const std::vector<u32>& scramble, const Call& call) {
        Cpu& c = m.cpu;
        for (int i = 0; i < 32; ++i) c.gpr[i] = scramble[i];
        c.gpr[1] = kStack;
        c.gpr[2] = kR2;
        c.gpr[13] = kR13;
        c.gpr[3] = call.r3;
        c.gpr[4] = call.r4;
        c.gpr[5] = call.r5;
        c.cr = scramble[32];
        c.ctr = scramble[33];
        c.xer = scramble[34] & 0xE0000000u;
        c.lr = kSentinel;
    }

    void fill_stack(unsigned seed) {
        std::mt19937 fill(seed);
        std::vector<u8> bytes(0x200);
        for (auto& v : bytes) v = static_cast<u8>(fill());
        a.bus.write_block(kStack - 0x1C0u, bytes.data(), bytes.size());
        b.bus.write_block(kStack - 0x1C0u, bytes.data(), bytes.size());
    }

    // Runs the call on both machines; fails on any difference. Returns r3.
    u32 call(const Call& call, bool compare_memory = true) {
        std::vector<u32> scramble(35);
        for (auto& v : scramble) v = rng();
        fill_stack(rng());
        arm(a, scramble, call);
        arm(b, scramble, call);
        a.cpu.pc = call.entry;
        a.cpu.npc = call.entry + 4u;
        unsigned long long steps = 0;
        while (a.cpu.pc != kSentinel && !a.cpu.stopped() && steps++ < 20'000'000ull) a.cpu.run_steps(1u);
        require(a.cpu.pc == kSentinel && !a.cpu.stopped(), "the original call at " + hex(call.entry) + " did not return");
        bool ok = true;
        switch (call.entry) {
            case kInitAlloc: ok = serve_init_alloc(b.cpu); break;
            case kCreateHeap: ok = serve_create_heap(b.cpu); break;
            case kSetCurrentHeap: serve_set_current_heap(b.cpu); break;
            case kAllocFromHeap: ok = serve_alloc_from_heap(b.cpu); break;
            case kFreeToHeap: ok = serve_free_to_heap(b.cpu); break;
            default: require(false, "unknown entry");
        }
        require(ok, "the native owner refused a call the original completed (" + hex(call.entry) + ")");
        ++calls;
        // The owner returns to LR; the original ran its blr to the sentinel.
        require(b.cpu.lr == kSentinel, "LR at the return differs (" + hex(call.entry) + ")");
        const Snapshot sa = snapshot(a.cpu), sb = snapshot(b.cpu);
        // What a caller can rely on: the result (OSFreeToHeap returns nothing) and the registers the EABI keeps.
        if (call.entry != kFreeToHeap)
            require(sa.gpr[3] == sb.gpr[3], "r3 differs after " + hex(call.entry) + ": original " + hex(sa.gpr[3]) +
                                                " native " + hex(sb.gpr[3]));
        for (int i : {1, 2, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31})
            require(sa.gpr[i] == sb.gpr[i], "callee-saved r" + std::to_string(i) + " differs after " + hex(call.entry));
        ++checks;
        compare_regions(call.entry, compare_memory);
        return sa.gpr[3];
    }

    void compare_regions(u32 entry, bool heap) {
        const auto same = [&](u32 address, u32 size, const char* what) {
            std::vector<u8> x(size), y(size);
            a.bus.read_block(address, x.data(), size);
            b.bus.read_block(address, y.data(), size);
            if (x != y) {
                u32 at = 0;
                while (x[at] == y[at]) ++at;
                require(false, std::string(what) + " differs after " + hex(entry) + " at " + hex(address + at));
            }
        };
        same(kR13 + 0x26E0u, 8u, "the current-heap cell");
        same(kR13 + 0x5A60u, 0x14u, "the heap globals");
        if (heap) same(heap_lo, heap_hi - heap_lo, "the heap memory");
        ++checks;
    }
};

struct Stats {
    unsigned long long alloc = 0, alloc_refused = 0, freed = 0, created = 0, create_refused = 0, init = 0, set_current = 0;
    unsigned long long exact = 0, boundary = 0;
};

// One heap with the game's shape: a one-descriptor array, one heap over the rest of the arena.
void run_heaps(Pair& p, Stats& st, u32 heaps, unsigned ops, unsigned full_compare_every) {
    p.call({kInitAlloc, kArenaLo, p.heap_hi + (p.rng() & 31u), heaps});
    ++st.init;
    // The array occupies the first heaps*12 bytes; the heap regions are carved by OSCreateHeap from what is left.
    const u32 array_end = (p.a.read32(kR13 + kArenaStart));
    const u32 end = p.a.read32(kR13 + kArenaEnd);
    require(array_end == p.b.read32(kR13 + kArenaStart), "ArenaStart differs");
    const u32 span = end - array_end;
    std::vector<std::pair<u32, u32>> regions;  // (start, end) of each heap region
    for (u32 h = 0; h < heaps + 1; ++h) {
        // Heap h gets an equal share of what is left (unaligned bounds after the first); the extra create finds no
        // descriptor free and is refused.
        const u32 share = (span / heaps) & ~31u;
        const u32 lo = h < heaps ? array_end + h * share + (h == 0 ? 0u : (p.rng() & 31u)) : array_end;
        const u32 hi = h + 1 == heaps ? end : (h < heaps ? array_end + (h + 1) * share - (p.rng() & 31u) : end);
        const u32 index = p.call({kCreateHeap, lo, hi, 0u});
        if (index == 0xFFFFFFFFu) ++st.create_refused; else ++st.created;
        regions.push_back({lo, hi});
    }
    const u32 current = p.call({kSetCurrentHeap, 0u, 0u, 0u});
    ++st.set_current;
    (void)current;
    p.call({kSetCurrentHeap, 0u, 0u, 0u});
    ++st.set_current;

    std::vector<std::pair<u32, u32>> live;  // (heap, pointer)
    const auto read32 = [&](u32 a) { return p.b.read32(a); };
    for (unsigned op = 0; op < ops; ++op) {
        const bool compare = full_compare_every == 0 || op % full_compare_every == 0;
        const u32 heap = p.rng() % heaps;
        const bool do_free = !live.empty() && (p.rng() % 100u) < 47u;
        if (do_free) {
            const std::size_t at = p.rng() % live.size();
            const auto victim = live[at];
            live[at] = live.back();
            live.pop_back();
            p.call({kFreeToHeap, victim.first, victim.second, 0u}, compare);
            ++st.freed;
        } else {
            u32 size;
            const u32 pick = p.rng() % 100u;
            if (pick < 4u) {
                static const u32 edges[] = {0u, 1u, 0x1Fu, 0x20u, 0x21u, 0x3Fu, 0x40u, 0x41u, 0x5Fu, 0x60u};
                size = edges[p.rng() % (sizeof(edges) / sizeof(edges[0]))];
            } else if (pick < 12u) {
                // Aim at a free cell: exact fits and leftovers around the 0x40 split boundary.
                std::string error;
                const u32 desc = p.b.read32(kR13 + kHeapArray) + heap * 12u;
                const auto cells = os_heap_list(read32, p.b.read32(desc + 4u), array_end, end, &error);
                if (!cells.empty()) {
                    const auto cell = cells[p.rng() % cells.size()];
                    static const u32 back[] = {0x20u, 0x40u, 0x60u, 0x3Fu, 0x41u, 0x5Fu, 0x80u};
                    const u32 k = back[p.rng() % (sizeof(back) / sizeof(back[0]))];
                    size = cell.second > k ? cell.second - k : 0u;
                    if (k == 0x20u) ++st.exact; else ++st.boundary;
                } else {
                    size = p.rng() % 600u;
                }
            } else if (pick < 94u) {
                size = 1u + p.rng() % 3000u;
            } else if (pick < 99u) {
                size = 1u + p.rng() % 60000u;
            } else {
                size = span;  // never fits a region
            }
            const u32 pointer = p.call({kAllocFromHeap, heap, size, 0u}, compare);
            ++st.alloc;
            if (pointer == 0u) ++st.alloc_refused; else live.push_back({heap, pointer});
        }
        if (op % 97u == 0u) {
            const OsHeapsCensus census = census_os_heaps(read32);
            require(census.error.empty(), "census: " + census.error);
            for (const auto& heap_census : census.census)
                require(heap_census.error.empty(), "census heap: " + heap_census.error);
        }
    }
    // Final full compare and drain: free everything, the heap must be one cell again.
    p.compare_regions(0u, true);
    for (const auto& item : live) p.call({kFreeToHeap, item.first, item.second, 0u}, false);
    p.compare_regions(0u, true);
    (void)regions;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: os_heap_contract_tests main.dol");
        std::ifstream input(argv[1], std::ios::binary);
        g_dol.assign(std::istreambuf_iterator<char>(input), {});
        require(g_dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : g_dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");

        Stats st;
        unsigned long long calls = 0, checks = 0;
        // 1. The game's shape: one descriptor, one heap; small (every byte compared after every call) and 8 MiB.
        for (unsigned seed : {1u, 2u, 3u}) {
            Pair p(seed, kArenaLo, kArenaLo + 0x20000u);
            run_heaps(p, st, 1u, 4000u, 0u);
            calls += p.calls; checks += p.checks;
        }
        {
            Pair p(11u, kArenaLo, kArenaLo + 0x800000u);
            run_heaps(p, st, 1u, 12000u, 64u);
            calls += p.calls; checks += p.checks;
        }
        // 2. Several heaps; more regions than descriptors (the last create is refused).
        for (unsigned seed : {21u, 22u}) {
            Pair p(seed, kArenaLo, kArenaLo + 0x40000u);
            run_heaps(p, st, 3u, 4000u, 0u);
            calls += p.calls; checks += p.checks;
        }
        // 3. Descriptor-count extremes of OSInitAlloc / OSCreateHeap: none, negative, one, unaligned bounds.
        for (u32 count : {0u, 0xFFFFFFFFu, 1u, 2u}) {
            for (unsigned variant = 0; variant < 8; ++variant) {
                Pair p(100u + variant, kArenaLo, kArenaLo + 0x10000u);
                p.call({kInitAlloc, kArenaLo + (p.rng() & 0x1Fu), kArenaLo + 0x10000u + (p.rng() & 0x1Fu), count});
                for (int i = 0; i < 4; ++i)
                    p.call({kCreateHeap, kArenaLo + 0x100u + (p.rng() & 0x7Fu), kArenaLo + 0x8000u + (p.rng() & 0xFFFFu), 0u});
                p.call({kSetCurrentHeap, p.rng(), 0u, 0u});
                calls += p.calls; checks += p.checks;
            }
        }
        // 4. Wrapped sizes: the original compares the rounded size as a signed word (single calls on a fresh heap).
        for (u32 size : {0x7FFFFFE0u, 0x7FFFFFE1u, 0xFFFFFFA0u, 0xFFFFFFC1u, 0xFFFFFFDFu, 0xFFFFFFE0u, 0xFFFFFFFFu}) {
            for (u32 prefill : {0u, 1u}) {
                Pair p(200u + size % 7u, kArenaLo, kArenaLo + 0x20000u);
                p.call({kInitAlloc, kArenaLo, kArenaLo + 0x20000u, 1u});
                const u32 start = p.a.read32(kR13 + kArenaStart);
                p.call({kCreateHeap, start, kArenaLo + 0x20000u, 0u});
                p.call({kSetCurrentHeap, 0u, 0u, 0u});
                if (prefill) p.call({kAllocFromHeap, 0u, 0x100u, 0u});
                p.call({kAllocFromHeap, 0u, size, 0u});
                calls += p.calls; checks += p.checks;
            }
        }
        std::printf("PASS os heap: %llu calls, %llu comparisons equal the original (r3, callee-saved registers, LR, heap memory,\n"
                    "  descriptors, small-data cells): init %llu, create %llu (%llu refused), set-current %llu,\n"
                    "  alloc %llu (%llu refused, %llu aimed at exact fits, %llu at the 0x40 boundary), free %llu\n",
                    calls, checks, st.init, st.created, st.create_refused, st.set_current, st.alloc, st.alloc_refused,
                    st.exact, st.boundary, st.freed);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL os heap contract: %s\n", error.what());
        return 1;
    }
}
