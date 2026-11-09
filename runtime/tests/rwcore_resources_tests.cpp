// Oracle for the native RenderWare resources arena (rwcore/src/resources.cpp, rw/core/resources.h): the ORIGINAL
// baresour / resmem functions of PAL main.dol in the reference interpreter (rwcore_oracle.h) and the native ones run
// the same random sessions.
//
// Part 1, the heap: _rwResHeapInit on memory at every start offset 0..31 from a 32-byte boundary where both sides
// lay the heap out alike (below) and sizes 0 .. 20 KB (the small ones failing), then random _rwResHeapAlloc (sizes 0
// .. beyond the heap, multiples of 32 and not) and _rwResHeapFree. After every call: the result (as an offset into the
// memory) and the whole heap - every block's offset, size, flags, previous block and heap pointer, and where
// allocations start looking.
//
// Part 2, the arena: each session opens the module with an arena size (0, too small for a heap, 96 .. 16 KB; now and
// then the arena's allocation failing), then runs operations: entries allocated (owners, with and without an owner
// pointer and a destroy callback; sizes up to beyond the arena, so allocations evict the free list's and the used
// list's oldest entries and sometimes fail with the arena empty), freed (in the arena and outside it: link.next NULL,
// RwFree), used (RwResourcesUseResEntry: RenderWare's release macro, inlined into the game's callers, so it is the
// test's own on the guest side), purged, the arena emptied; the module closed. After every operation: every result,
// the destroy callbacks in order, the owner pointers, both lists (entries in order, both directions), which list is
// the free one, maxSize / currentSize / reusageSize, every entry's fields and its place in the heap, the heap itself,
// the arena's memory requests (size, hint), the frees of entries outside the arena and the held error.
//
// Domains (the 64-bit layouts, rwcore/README.md): the native heap's header (rwResHeap, two pointers) is 16 bytes,
// the GameCube's 8, so the first block lands on the same boundary only for memory 0..16 or 25..31 bytes past a 32-byte
// boundary (every 8- and 16-byte aligned allocation is one of those); a native RwResEntry is 48 bytes, the GameCube's
// 24, so an entry takes the same heap block on both only when its size is 9..16 modulo 32 - the sizes the arena part
// draws. Not generated (rwcore_resources_native_tests): other offsets and sizes, negative sizes and those whose
// rounding wraps (the original allocates blocks too small for them), an arena of size 0 being allocated from (the
// original reads through NULL), a purge with no module open.
// liveness-roots: reference-interpreter-only
//
// Usage: rwcore_resources_tests <main.dol>
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/resources.h"
#include "rwcore_oracle.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
using namespace rworacle;

constexpr Original kResOpen{"_rwResourcesOpen", 0x8048158Cu, 0x154u, 0x9421FFD0u, 0x4E800020u};
constexpr Original kResClose{"_rwResourcesClose", 0x804816E0u, 0x8Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kFreeEntry{"RwResourcesFreeResEntry", 0x8048176Cu, 0xB0u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kPurge{"_rwResourcesPurge", 0x8048181Cu, 0xACu, 0x806D615Cu, 0x4E800020u};
constexpr Original kAllocEntry{"RwResourcesAllocateResEntry", 0x804818C8u, 0x290u, 0x9421FFD0u, 0x4E800020u};
constexpr Original kEmptyArena{"RwResourcesEmptyArena", 0x80481B58u, 0x164u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kHeapInit{"_rwResHeapInit", 0x804839D4u, 0x50u, 0x38A30027u, 0x4E800020u};
constexpr Original kHeapClose{"_rwResHeapClose", 0x80483A24u, 0x8u, 0x38600001u, 0x4E800020u};
constexpr Original kHeapFree{"_rwResHeapFree", 0x80483A2Cu, 0xB8u, 0x3883FFE0u, 0x4E800020u};
constexpr Original kHeapAlloc{"_rwResHeapAlloc", 0x80483AE4u, 0x130u, 0x3804001Fu, 0x4E800020u};
constexpr Original kErrorOpen{"_rwErrorOpen", 0x8047EB88u, 0x38u, 0x390D6100u, 0x4E800020u};
constexpr Original kErrorGet{"RwErrorGet", 0x8047EC34u, 0x44u, 0x808D615Cu, 0x4E800020u};
constexpr u32 kResourcesModule = 0x805F2620u;
constexpr u32 kGuestErrorOffset = 0x200u, kGuestResOffset = 0x300u, kGuestResSize = 0x28u;
constexpr u32 kRes = kEngine + kGuestResOffset;
constexpr u32 kResArenaInitSize = kEngine + 0x128u;   // RwGlobals::resArenaInitSize (GameCube layout)
constexpr u32 kArenaHint = 0x0004040Bu;
constexpr u32 kNotify = 0x817F0100u;                   // the destroy callback (stub)
constexpr u32 kErr = kBuffer + 0x20u, kSlots = kBuffer + 0x100u;
constexpr u32 kHeapArea = 0x81710000u, kHeapAreaSize = 0x8000u;
constexpr int kSlotCount = 48;

// ---- the native side's memory: 32-byte aligned (as the guest heap), every request recorded -------------------------
struct Request {
    std::size_t size;
    RwUInt32 hint;
    bool operator==(const Request&) const = default;
};
std::vector<Request> g_nmallocs;
std::vector<void*> g_nfrees;
long g_nfailMalloc = -1;
void* alignedAlloc(std::size_t size) {
    char* const raw = static_cast<char*>(std::malloc(size + 64));
    if (raw == nullptr) return nullptr;
    char* const p = reinterpret_cast<char*>((reinterpret_cast<std::uintptr_t>(raw) + 32 + 31) & ~std::uintptr_t{31});
    std::memcpy(p - sizeof(void*), &raw, sizeof raw);
    return p;
}
void alignedFree(void* p) {
    if (p == nullptr) return;
    void* raw;
    std::memcpy(&raw, static_cast<char*>(p) - sizeof(void*), sizeof raw);
    std::free(raw);
}
void* nMalloc(std::size_t size, RwUInt32 hint) {
    const long index = static_cast<long>(g_nmallocs.size());
    g_nmallocs.push_back({size, hint});
    return index == g_nfailMalloc ? nullptr : alignedAlloc(size ? size : 1);
}
void nFree(void* p) {
    g_nfrees.push_back(p);
    alignedFree(p);
}
void* nRealloc(void* p, std::size_t size, RwUInt32) {
    void* const fresh = alignedAlloc(size ? size : 1);
    if (fresh != nullptr && p != nullptr) std::memcpy(fresh, p, size);   // the tests do not reallocate
    alignedFree(p);
    return fresh;
}
void* nCalloc(std::size_t n, std::size_t size, RwUInt32) {
    void* const p = alignedAlloc(n * size ? n * size : 1);
    if (p != nullptr) std::memset(p, 0, n * size);
    return p;
}

std::map<const void*, int> g_nids;
int nid(const void* p) {
    if (p == nullptr) return -10;
    const auto it = g_nids.find(p);
    return it == g_nids.end() ? -1 : it->second;
}
std::vector<int> g_nnotified;
void nNotify(RwResEntry* entry) { g_nnotified.push_back(nid(entry)); }

// One heap block, as both sides report it (offsets from the heap memory; -1 for none).
struct Block {
    long long offset, prev;
    u32 size, flags;
    bool heapOk;
    bool operator==(const Block&) const = default;
};
std::string show(const std::vector<Block>& v, long long firstFree) {
    std::string s = " firstFree " + std::to_string(firstFree) + ":";
    for (const Block& b : v) {
        s += " [" + std::to_string(b.offset) + " size " + std::to_string(b.size) + " flags " + std::to_string(b.flags) +
             " prev " + std::to_string(b.prev) + (b.heapOk ? "" : " BADHEAP") + "]";
    }
    return s;
}
std::string show(const std::vector<int>& v) {
    std::string s;
    for (int x : v) s += " " + std::to_string(x);
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rwcore_resources_tests main.dol");
        Guest g(argv[1]);
        for (const Original& f : {kResOpen, kResClose, kFreeEntry, kPurge, kAllocEntry, kEmptyArena, kHeapInit,
                                  kHeapClose, kHeapFree, kHeapAlloc, kErrorOpen, kErrorGet}) {
            g.check_pinned(f);
        }
        GuestMemory& m = g.memory();
        Rng rng;
        u64 instructions = 0;
        unsigned checks = 0;

        // the heap as each side sees it
        const auto guestHeap = [&](u32 base, long long& firstFree) {
            std::vector<Block> v;
            const u32 ff = m.peek32(base + 4);
            firstFree = ff ? static_cast<long long>(ff) - base : -1;
            for (u32 b = m.peek32(base); b != 0 && v.size() < 100000; b = m.peek32(b + 4)) {
                const u32 prev = m.peek32(b + 8);
                v.push_back({static_cast<long long>(b) - base, prev ? static_cast<long long>(prev) - base : -1,
                             m.peek32(b + 0xC), m.peek32(b + 0x10), m.peek32(b) == base});
            }
            return v;
        };
        const auto nativeHeap = [](void* memory, long long& firstFree) {
            std::vector<Block> v;
            const auto* const heap = static_cast<const rwResHeap*>(memory);
            const auto off = [&](const void* p) {
                return p ? static_cast<long long>(static_cast<const char*>(p) - static_cast<const char*>(memory)) : -1;
            };
            firstFree = off(heap->firstFree);
            for (const rwResHeapBlockHeader* b = heap->firstBlock; b != nullptr && v.size() < 100000; b = b->next) {
                v.push_back({off(b), off(b->prev), b->size, b->flags, b->heap == heap});
            }
            return v;
        };
        const auto sameHeap = [&](u32 gbase, void* nbase, const std::string& what) {
            long long gff = 0, nff = 0;
            const std::vector<Block> gv = guestHeap(gbase, gff), nv = nativeHeap(nbase, nff);
            require(gv == nv && gff == nff, what + ": the heaps differ; original" + show(gv, gff) + ", native" + show(nv, nff));
        };

        // ---- part 1: the heap -----------------------------------------------------------------------------------
        unsigned heaps = 0, heapFailures = 0, allocs = 0, allocFailures = 0, splits = 0, frees = 0;
        {
            char* const nArea = static_cast<char*>(alignedAlloc(kHeapAreaSize + 64));
            for (unsigned session = 0; session < 600; ++session) {
                const std::string sw = "heap session " + std::to_string(session);
                u32 k = rng.next() % 32u;
                if (k >= 17u && k <= 24u) k -= 17u;   // the offsets where both lay the heap out alike (see the top)
                const u32 r = rng.next() % 8u;
                const u32 size = r == 0 ? rng.next() % 140u : r == 1 ? 60u + rng.next() % 50u
                                                            : 96u + rng.next() % 20000u;
                const u32 gbase = kHeapArea + k;
                void* const nbase = nArea + k;
                instructions += g.call(kHeapInit.entry, {gbase, size});
                const RwBool nok = _rwResHeapInit(nbase, size);
                require((g.r3() != 0) == (nok != 0) && g.r3() <= 1u, sw + ": _rwResHeapInit");
                ++checks;
                if (!nok) {
                    ++heapFailures;
                    continue;
                }
                ++heaps;
                sameHeap(gbase, nbase, sw + " (init)");
                std::vector<std::pair<u32, void*>> live;
                const unsigned ops = 20 + rng.next() % 200u;
                for (unsigned op = 0; op < ops; ++op) {
                    if (live.empty() || rng.next() % 5u < 3u) {
                        const u32 s = rng.next() % 9u == 0 ? size + rng.next() % 64u
                                    : rng.next() % 4u == 0 ? 32u * (rng.next() % 20u)
                                                           : rng.next() % (1u + size / 4u);
                        long long ignored = 0;
                        const std::size_t before = guestHeap(gbase, ignored).size();
                        instructions += g.call(kHeapAlloc.entry, {gbase, s});
                        void* const n = _rwResHeapAlloc(nbase, s);
                        const u32 gp = g.r3();
                        require((gp == 0) == (n == nullptr), sw + ": _rwResHeapAlloc result");
                        if (gp != 0) {
                            require(static_cast<long long>(gp) - gbase ==
                                        static_cast<char*>(n) - static_cast<char*>(nbase),
                                    sw + ": _rwResHeapAlloc's place");
                            live.push_back({gp, n});
                            ++allocs;
                            if (guestHeap(gbase, ignored).size() > before) ++splits;
                        } else {
                            ++allocFailures;
                        }
                    } else {
                        const std::size_t i = rng.next() % live.size();
                        instructions += g.call(kHeapFree.entry, {live[i].first});
                        _rwResHeapFree(live[i].second);
                        live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
                        ++frees;
                    }
                    sameHeap(gbase, nbase, sw + " op " + std::to_string(op));
                    ++checks;
                }
                instructions += g.call(kHeapClose.entry, {gbase});
                require(g.r3() == 1u && _rwResHeapClose(nbase) == 1, sw + ": _rwResHeapClose");
            }
            alignedFree(nArea);
        }
        require(heaps > 400 && heapFailures > 50 && allocs > 20000 && allocFailures > 1000 && splits > 10000 &&
                    frees > 10000,
                "the heap sessions must reach every case");
        std::printf("  heap: %u heaps (+%u too small), %u allocations (%u split, %u failed), %u frees\n", heaps,
                    heapFailures, allocs, splits, allocFailures, frees);

        // ---- part 2: the arena ----------------------------------------------------------------------------------
        std::vector<int> gnotified;
        std::map<u32, int> gids;
        const auto gid = [&](u32 a) {
            if (a == 0) return -10;
            const auto it = gids.find(a);
            return it == gids.end() ? -1 : it->second;
        };
        g.stub(kNotify, [&](Cpu& c) { gnotified.push_back(gid(c.gpr[3])); });

        g.reset_engine();
        for (u32 a : {kErrorModule, kErrorModule + 4u, kResourcesModule, kResourcesModule + 4u}) m.write32(a, 0);
        g.call(kErrorOpen.entry, {kEngine, kGuestErrorOffset, 8});

        const RwMemoryFunctions funcs{nMalloc, nFree, nRealloc, nCalloc};
        RwEngineOpenParams params{nullptr};
        require(RwEngineInit(&funcs, 0, 0) == 1, "native init");
        require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "native start");
        void* const nativeInstance = RwEngineInstance;
        const RwInt32 nativeOffset = resourcesModule.globalsOffset;
        rwResources& N = rwResourcesArena();
        // the native module is open (RwEngineStart, an arena of size 0): close it, the sessions open it as the guest's
        _rwResourcesClose(nativeInstance, nativeOffset, sizeof(rwResourcesGlobals));
        require(resourcesModule.numInstances == 0, "the native module closed");

        std::vector<RwResEntry*> nslots(kSlotCount, nullptr);
        std::vector<RwResEntry*> nById;   // the native entry of each id
        std::size_t gmallocSeen = 0, gfreeSeen = 0;
        const auto sameRequests = [&](const std::string& what) {
            std::vector<Request> gm, nm;
            for (std::size_t i = gmallocSeen; i < g.mallocs.size(); ++i) gm.push_back({g.mallocs[i].size, g.mallocs[i].hint});
            nm = g_nmallocs;
            const auto list = [](const std::vector<Request>& v) {
                std::string s;
                for (const Request& r : v) s += " " + std::to_string(r.size) + "/" + std::to_string(r.hint);
                return s;
            };
            require(gm == nm, what + ": memory requests differ; original" + list(gm) + ", native" + list(nm));
            gmallocSeen = g.mallocs.size();
            g_nmallocs.clear();
            g.fail_call = -1, g_nfailMalloc = -1;
        };
        const auto sameErrors = [&](const std::string& what) {
            instructions += g.call(kErrorGet.entry, {kErr});
            RwError e{};
            RwErrorGet(&e);
            char buf[96];
            std::snprintf(buf, sizeof buf, ": errors (original %08X/%08X, native %08X/%08X)", m.peek32(kErr),
                          m.peek32(kErr + 4), static_cast<u32>(e.pluginID), static_cast<u32>(e.errorCode));
            require(m.peek32(kErr) == static_cast<u32>(e.pluginID) && m.peek32(kErr + 4) == static_cast<u32>(e.errorCode),
                    what + buf);
        };
        // an entry's owner pointer slot (-1: none)
        const auto gslotOf = [](u32 ref) { return ref ? static_cast<int>((ref - kSlots) / 4u) : -1; };
        const auto nslotOf = [&](RwResEntry** ref) { return ref ? static_cast<int>(ref - nslots.data()) : -1; };
        const auto walk = [&](u32 head, bool forward) {
            std::vector<int> ids;
            for (u32 l = m.peek32(head + (forward ? 0u : 4u)); l != head && ids.size() < 100000; l = m.peek32(l + (forward ? 0u : 4u))) {
                ids.push_back(gid(l));
            }
            return ids;
        };
        const auto nwalk = [&](RwLinkList* list, bool forward) {
            std::vector<int> ids;
            for (RwLLLink* l = forward ? list->link.next : list->link.prev; l != &list->link && ids.size() < 100000;
                 l = forward ? l->next : l->prev) {
                ids.push_back(nid(l));
            }
            return ids;
        };
        const auto sameState = [&](bool open, const std::string& what) {
            require(gnotified == g_nnotified, what + ": destroy callbacks differ; original" + show(gnotified) +
                                                  ", native" + show(g_nnotified));
            gnotified.clear(), g_nnotified.clear();
            sameErrors(what);
            sameRequests(what);
            require(static_cast<RwInt32>(m.peek32(kResourcesModule + 4)) == resourcesModule.numInstances,
                    what + ": numInstances");
            for (int s = 0; s < kSlotCount; ++s) {
                require(gid(m.peek32(kSlots + 4u * static_cast<u32>(s))) == nid(nslots[static_cast<std::size_t>(s)]),
                        what + ": owner pointer " + std::to_string(s));
            }
            if (!open) return;
            require(static_cast<RwInt32>(m.peek32(kRes)) == N.maxSize && static_cast<RwInt32>(m.peek32(kRes + 4)) == N.currentSize &&
                        static_cast<RwInt32>(m.peek32(kRes + 8)) == N.reusageSize,
                    what + ": sizes (original " + std::to_string(static_cast<RwInt32>(m.peek32(kRes))) + "/" +
                        std::to_string(static_cast<RwInt32>(m.peek32(kRes + 4))) + "/" +
                        std::to_string(static_cast<RwInt32>(m.peek32(kRes + 8))) + ", native " + std::to_string(N.maxSize) +
                        "/" + std::to_string(N.currentSize) + "/" + std::to_string(N.reusageSize) + ")");
            const u32 gheap = m.peek32(kRes + 0xC);
            require((gheap == 0) == (N.memHeap == nullptr), what + ": memHeap");
            const bool gfreeIsB = m.peek32(kRes + 0x20) == kRes + 0x18u;
            require(gfreeIsB == (N.freeEntries == &N.entriesB) && m.peek32(kRes + 0x24) == (gfreeIsB ? kRes + 0x10u : kRes + 0x18u) &&
                        N.usedEntries == (gfreeIsB ? &N.entriesA : &N.entriesB),
                    what + ": the lists' roles");
            for (int list = 0; list < 2; ++list) {
                const u32 ghead = m.peek32(kRes + (list ? 0x24u : 0x20u));
                RwLinkList* const nhead = list ? N.usedEntries : N.freeEntries;
                const std::vector<int> gf = walk(ghead, true), nf = nwalk(nhead, true);
                std::vector<int> gb = walk(ghead, false), nb = nwalk(nhead, false);
                std::reverse(gb.begin(), gb.end()), std::reverse(nb.begin(), nb.end());
                require(gf == nf && gb == nb && gf == gb, what + (list ? ": used" : ": free") + " list differs; original" +
                                                              show(gf) + " /" + show(gb) + ", native" + show(nf) + " /" + show(nb));
                for (u32 l = m.peek32(ghead); l != ghead; l = m.peek32(l)) {
                    const int id = gid(l);
                    require(id >= 0, what + ": an entry the test did not make");
                    RwResEntry* const n = nById[static_cast<std::size_t>(id)];
                    require(static_cast<RwInt32>(m.peek32(l + 8)) == n->size &&
                                m.peek32(l + 0xC) == static_cast<u32>(reinterpret_cast<std::uintptr_t>(n->owner)) &&
                                gslotOf(m.peek32(l + 0x10)) == nslotOf(n->ownerRef) &&
                                (m.peek32(l + 0x14) != 0) == (n->destroyNotify != nullptr),
                            what + ": entry " + std::to_string(id) + "'s fields");
                    require(gheap && static_cast<long long>(l) - gheap ==
                                         reinterpret_cast<char*>(n) - static_cast<char*>(N.memHeap),
                            what + ": entry " + std::to_string(id) + "'s place in the heap");
                }
            }
            if (gheap) sameHeap(gheap, N.memHeap, what);
            ++checks;
        };

        {
            RwError e{};
            RwErrorGet(&e);   // whatever the engine's start left
        }
        g_nmallocs.clear(), g_nfrees.clear();
        gmallocSeen = g.mallocs.size(), gfreeSeen = g.frees.size();
        unsigned sessions = 0, failedOpens = 0, noArena = 0, entries = 0, failedEntries = 0, evictedFree = 0,
                 evictedUsed = 0, freed = 0, outside = 0, used = 0, purges = 0, empties = 0;
        int nextId = 0;
        for (unsigned session = 0; session < 320; ++session) {
            const std::string sw = "arena session " + std::to_string(session);
            // ---- open ---------------------------------------------------------------------------------------------
            const u32 r = rng.next() % 16u;
            const u32 arenaSize = r == 0 ? 0u : r == 1 ? rng.next() % 96u : r == 2 ? 96u + rng.next() % 64u
                                                                       : 96u + rng.next() % 16384u;
            m.write32(kResArenaInitSize, arenaSize);
            rwSrcGlobals().resArenaInitSize = arenaSize;
            const bool failAlloc = arenaSize != 0 && rng.next() % 10u == 0;
            if (failAlloc) g.fail_call = static_cast<int>(g.mallocs.size()), g_nfailMalloc = 0;
            instructions += g.call(kResOpen.entry, {kEngine, kGuestResOffset, kGuestResSize});
            void* const nopen = _rwResourcesOpen(nativeInstance, nativeOffset, sizeof(rwResourcesGlobals));
            require((g.r3() == kEngine) == (nopen == nativeInstance) && (g.r3() == 0) == (nopen == nullptr),
                    sw + ": _rwResourcesOpen");
            if (g.r3() == 0) {
                // the original freed the arena memory but left memHeap pointing at it (native: cleared)
                const u32 gheap = m.peek32(kRes + 0xC);
                require(gheap == 0 || std::find(g.frees.begin() + static_cast<std::ptrdiff_t>(gfreeSeen), g.frees.end(), gheap) != g.frees.end(),
                        sw + ": the failed open's memory");
                require(N.memHeap == nullptr && g_nfrees.size() == (gheap != 0 ? 1u : 0u),
                        sw + ": the native failed open's memory");
                m.write32(kRes + 0xC, 0);
                sameState(false, sw + " (failed open)");
                gfreeSeen = g.frees.size(), g_nfrees.clear();
                ++failedOpens;
                continue;
            }
            if (arenaSize == 0) ++noArena;
            sameState(true, sw + " (open)");
            for (int s = 0; s < kSlotCount; ++s) {
                m.write32(kSlots + 4u * static_cast<u32>(s), 0);
                nslots[static_cast<std::size_t>(s)] = nullptr;
            }
            std::vector<std::pair<u32, RwResEntry*>> outsiders;   // entries outside the arena, not yet freed
            const auto liveEntries = [&]() {
                std::vector<std::pair<u32, RwResEntry*>> v;
                for (int list = 0; list < 2; ++list) {
                    const u32 ghead = m.peek32(kRes + (list ? 0x24u : 0x20u));
                    RwLLLink* const nhead = &(list ? N.usedEntries : N.freeEntries)->link;
                    RwLLLink* nl = nhead->next;
                    for (u32 l = m.peek32(ghead); l != ghead; l = m.peek32(l), nl = nl->next) {
                        v.push_back({l, reinterpret_cast<RwResEntry*>(nl)});
                    }
                }
                return v;
            };
            const unsigned ops = arenaSize == 0 ? 6u : 30u + rng.next() % 400u;
            for (unsigned op = 0; op < ops; ++op) {
                const std::string what = sw + " op " + std::to_string(op);
                u32 kind = rng.next() % 100u;
                if (arenaSize == 0 && kind < 60u) kind = 70u;   // no allocations from an arena of size 0 (native: refused)
                if (kind < 60u) {
                    // ---- allocate -----------------------------------------------------------------------------------
                    const u32 big = rng.next() % 12u;
                    const u32 limit = big == 0 ? arenaSize + 256u : big < 4u ? arenaSize / 2u + 32u : arenaSize / 8u + 32u;
                    const u32 size = 32u * (rng.next() % (limit / 32u + 1u)) + 9u + rng.next() % 8u;   // 9..16 mod 32
                    const int slot = rng.next() % 4u == 0 ? -1 : static_cast<int>(rng.next() % kSlotCount);
                    const bool notify = rng.next() % 3u != 0;
                    const u32 owner = 0x1000u + rng.next() % 0x1000u;
                    const u32 gref = slot < 0 ? 0u : kSlots + 4u * static_cast<u32>(slot);
                    RwResEntry** const nref = slot < 0 ? nullptr : &nslots[static_cast<std::size_t>(slot)];
                    const std::vector<std::pair<u32, RwResEntry*>> before = liveEntries();
                    const RwInt32 usedBefore = static_cast<RwInt32>(m.peek32(kRes + 8));
                    instructions += g.call(kAllocEntry.entry, {owner, gref, size, notify ? kNotify : 0u});
                    RwResEntry* const n = RwResourcesAllocateResEntry(reinterpret_cast<void*>(std::uintptr_t{owner}), nref,
                                                                      static_cast<RwInt32>(size), notify ? nNotify : nullptr);
                    const u32 ge = g.r3();
                    require((ge == 0) == (n == nullptr), what + ": RwResourcesAllocateResEntry result");
                    if (ge) {
                        gids[ge] = nextId, g_nids[n] = nextId, ++nextId, ++entries;
                        nById.push_back(n);
                    } else {
                        ++failedEntries;
                    }
                    const std::size_t after = liveEntries().size() - (ge ? 1u : 0u);
                    if (after < before.size()) {
                        if (static_cast<RwInt32>(m.peek32(kRes + 8)) > usedBefore) ++evictedUsed; else ++evictedFree;
                    }
                } else if (kind < 72u) {
                    // ---- free an entry --------------------------------------------------------------------------
                    std::vector<std::pair<u32, RwResEntry*>> live = liveEntries();
                    if (live.empty()) continue;
                    const auto [ge, ne] = live[rng.next() % live.size()];
                    instructions += g.call(kFreeEntry.entry, {ge});
                    require(g.r3() == 1u && RwResourcesFreeResEntry(ne) == 1, what + ": RwResourcesFreeResEntry");
                    ++freed;
                } else if (kind < 78u) {
                    // ---- an entry outside the arena, freed (now or later) ---------------------------------------
                    if (outsiders.empty() || rng.next() % 2u == 0) {
                        const int slot = static_cast<int>(rng.next() % kSlotCount);
                        const u32 ge = g.heap_alloc(0x18u);
                        const u32 owner = 0x2000u + rng.next() % 0x1000u, size = rng.next() % 500u;
                        const bool notify = rng.next() % 2u == 0;
                        m.write32(ge, 0), m.write32(ge + 4, 0), m.write32(ge + 8, size), m.write32(ge + 0xC, owner);
                        m.write32(ge + 0x10, kSlots + 4u * static_cast<u32>(slot)), m.write32(ge + 0x14, notify ? kNotify : 0u);
                        m.write32(kSlots + 4u * static_cast<u32>(slot), ge);
                        auto* const ne = static_cast<RwResEntry*>(RwMalloc(sizeof(RwResEntry), 0));
                        *ne = RwResEntry{{nullptr, nullptr}, static_cast<RwInt32>(size),
                                         reinterpret_cast<void*>(std::uintptr_t{owner}),
                                         &nslots[static_cast<std::size_t>(slot)], notify ? nNotify : nullptr};
                        nslots[static_cast<std::size_t>(slot)] = ne;
                        gids[ge] = nextId, g_nids[ne] = nextId, ++nextId;
                        nById.push_back(ne);
                        g_nmallocs.clear();   // the test's own, not the arena's
                        gmallocSeen = g.mallocs.size();
                        outsiders.push_back({ge, ne});
                        continue;
                    }
                    const std::size_t i = rng.next() % outsiders.size();
                    const auto [ge, ne] = outsiders[i];
                    outsiders.erase(outsiders.begin() + static_cast<std::ptrdiff_t>(i));
                    gfreeSeen = g.frees.size(), g_nfrees.clear();
                    instructions += g.call(kFreeEntry.entry, {ge});
                    require(g.r3() == 1u && RwResourcesFreeResEntry(ne) == 1, what + ": RwResourcesFreeResEntry (outside)");
                    require(g.frees.size() == gfreeSeen + 1 && g.frees.back() == ge && g_nfrees.size() == 1 && g_nfrees[0] == ne,
                            what + ": the outside entry's RwFree");
                    ++outside;
                } else if (kind < 90u) {
                    // ---- use an entry (RenderWare's macro on both sides) ---------------------------------------
                    std::vector<std::pair<u32, RwResEntry*>> live = liveEntries();
                    if (!outsiders.empty() && rng.next() % 6u == 0) live.push_back(outsiders[rng.next() % outsiders.size()]);
                    if (live.empty()) continue;
                    const auto [ge, ne] = live[rng.next() % live.size()];
                    if (m.peek32(ge) != 0) {
                        const u32 next = m.peek32(ge), prev = m.peek32(ge + 4);
                        m.write32(prev, next), m.write32(next + 4, prev);
                        const u32 head = m.peek32(kRes + 0x24);
                        m.write32(ge, m.peek32(head)), m.write32(ge + 4, head);
                        m.write32(m.peek32(head) + 4, ge), m.write32(head, ge);
                    }
                    require(RwResourcesUseResEntry(ne) == ne, what + ": RwResourcesUseResEntry");
                    ++used;
                } else if (kind < 97u) {
                    instructions += g.call(kPurge.entry, {});
                    _rwResourcesPurge();
                    ++purges;
                } else {
                    instructions += g.call(kEmptyArena.entry, {});
                    require(g.r3() == 1u && RwResourcesEmptyArena() == 1, what + ": RwResourcesEmptyArena");
                    ++empties;
                }
                sameState(true, what);
            }
            // ---- close: the outside entries freed, then the module -----------------------------------------------
            for (const auto& [ge, ne] : outsiders) {
                instructions += g.call(kFreeEntry.entry, {ge});
                RwResourcesFreeResEntry(ne);
            }
            sameState(true, sw + " (outside entries freed)");
            const u32 gheap = m.peek32(kRes + 0xC);
            void* const nheap = N.memHeap;
            gfreeSeen = g.frees.size(), g_nfrees.clear();
            instructions += g.call(kResClose.entry, {kEngine, kGuestResOffset, kGuestResSize});
            require(g.r3() == kEngine && _rwResourcesClose(nativeInstance, nativeOffset, sizeof(rwResourcesGlobals)) == nativeInstance,
                    sw + ": _rwResourcesClose");
            require(m.peek32(kRes + 0xC) == 0 && N.memHeap == nullptr, sw + ": the closed arena's memHeap");
            require((gheap == 0 ? g.frees.size() == gfreeSeen : g.frees.size() == gfreeSeen + 1 && g.frees.back() == gheap) &&
                        (nheap == nullptr ? g_nfrees.empty() : g_nfrees.size() == 1 && g_nfrees[0] == nheap),
                    sw + ": the arena memory freed");
            require(walk(kRes + 0x10, true).empty() && walk(kRes + 0x18, true).empty() && nwalk(&N.entriesA, true).empty() &&
                        nwalk(&N.entriesB, true).empty(),
                    sw + ": the closed arena's lists");
            sameState(false, sw + " (close)");
            gfreeSeen = g.frees.size(), g_nfrees.clear();
            ++sessions;
        }
        // the native module open again (an arena of size 0) for the engine's own close
        rwSrcGlobals().resArenaInitSize = 0;
        require(_rwResourcesOpen(nativeInstance, nativeOffset, sizeof(rwResourcesGlobals)) == nativeInstance, "the final open");
        require(RwEngineStop() == 1 && RwEngineClose() == 1 && RwEngineTerm() == 1, "native shutdown");
        std::printf("  arena: sessions %u (+%u failed opens, %u without an arena), entries %u (+%u failed), evictions "
                    "from the free list %u / used list %u, freed %u, outside %u, used %u, purges %u, emptied %u\n",
                    sessions, failedOpens, noArena, entries, failedEntries, evictedFree, evictedUsed, freed, outside, used,
                    purges, empties);
        require(sessions > 220 && failedOpens >= 35 && noArena >= 12 && entries > 26000 && failedEntries > 1400 &&
                    evictedFree > 1900 && evictedUsed > 5000 && freed > 4700 && outside > 1050 && used > 4800 &&
                    purges > 3200 && empties > 1350,
                "the arena sessions must reach every case");
        std::printf("rwcore_resources_tests: %u operations identical to the original (%llu original instructions)\n",
                    checks, static_cast<unsigned long long>(instructions));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_resources_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
