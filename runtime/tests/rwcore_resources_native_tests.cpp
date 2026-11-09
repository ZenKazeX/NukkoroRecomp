// Native tests of the RenderWare resources arena (rwcore/src/resources.cpp, rw/core/resources.h) for what the oracle
// cannot cover:
// - the engine's life: the module in its slot (an arena of RwEngineInit's size opened at RwEngineStart, memory hint
//   rwMEMHINTDUR_GLOBAL | rwID_RESOURCESMODULE; freed at RwEngineStop with every entry's destroy callback); the
//   arena's allocation failing and an arena too small for a heap (the start fails, nothing leaked, memHeap cleared);
//   the end-of-frame purge in RwCameraShowRaster;
// - the deliberate differences of rw/core/resources.h: negative sizes and an arena of size 0 refused at once (no
//   eviction, *ownerRef NULL, E_RW_INSUFFICIENTRESOURCES), a purge with no module open doing nothing;
// - the 64-bit layout outside the oracle's domain: heaps at every start offset 0..31 (the 16-byte rwResHeap always
//   before the first block), every entry size 0..300 and random ones (the 48-byte RwResEntry), sizes past RwUInt32
//   refused by the heap without writing, an arena's capacity and data alignment (16 bytes for the data after an entry);
// - invariants of random sessions (any size): the blocks tile the heap exactly (no gap, no overlap, no two free
//   blocks side by side, everything before firstFree used), each listed entry starts a used block, owner pointers
//   point at their entries, currentSize is the listed entries' sizes, data written fills its entry without touching the
//   heap, an allocation fails only when no free block holds it after every entry is gone.
// Memory is counted: every part ends with no live allocation.
//
// Usage: rwcore_resources_native_tests
#include "rw/core/camera.h"
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/resources.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
unsigned g_checks = 0;
void check(bool condition, const std::string& message) {
    require(condition, message);
    ++g_checks;
}

struct Live {
    std::size_t size;
    RwUInt32 hint;
};
std::map<void*, Live> g_live;
long g_mallocs = 0, g_failAt = -1;
void* countMalloc(std::size_t size, RwUInt32 hint) {
    if (g_mallocs++ == g_failAt) return nullptr;
    void* p = std::malloc(size ? size : 1);
    g_live[p] = {size, hint};
    return p;
}
void countFree(void* p) {
    if (p == nullptr) return;
    require(g_live.erase(p) == 1, "free of a pointer the allocator did not hand out");
    std::free(p);
}
void* countRealloc(void* p, std::size_t size, RwUInt32 hint) {
    if (p == nullptr) return countMalloc(size, hint);
    if (g_mallocs++ == g_failAt) return nullptr;
    void* q = std::realloc(p, size ? size : 1);
    if (q == nullptr) return nullptr;
    g_live.erase(p);
    g_live[q] = {size, hint};
    return q;
}
void* countCalloc(std::size_t n, std::size_t size, RwUInt32 hint) {
    void* p = countMalloc(n * size, hint);
    if (p != nullptr) std::memset(p, 0, n * size);
    return p;
}
const RwMemoryFunctions kFuncs{countMalloc, countFree, countRealloc, countCalloc};

RwError takeError() {
    RwError e{};
    RwErrorGet(&e);
    return e;
}
bool isError(const RwError& e, RwInt32 code) { return e.pluginID == rwID_COREPLUGIN && e.errorCode == code; }

std::vector<RwResEntry*> g_notified;
void notify(RwResEntry* entry) { g_notified.push_back(entry); }

void startEngine(RwUInt32 arenaSize) {
    RwEngineOpenParams params{nullptr};
    require(RwEngineInit(&kFuncs, 0, arenaSize) == 1 && RwEngineOpen(&params) == 1, "engine init / open");
    require(RwEngineStart() == 1, "engine start");
}
void stopEngine() {
    require(RwEngineStop() == 1 && RwEngineClose() == 1 && RwEngineTerm() == 1, "engine shutdown");
    require(g_live.empty(), "memory left after the engine's shutdown: " + std::to_string(g_live.size()) + " allocations");
}

/// The heap invariants; returns the bytes of the largest free block.
RwUInt32 checkHeap(void* memory, std::size_t size, const std::string& what) {
    const auto* heap = static_cast<const rwResHeap*>(memory);
    const auto base = reinterpret_cast<std::uintptr_t>(memory);
    const std::uintptr_t first = (base + sizeof(rwResHeap) + 31) & ~std::uintptr_t{31};
    const std::uintptr_t end = (base + size) & ~std::uintptr_t{31};
    require(reinterpret_cast<std::uintptr_t>(heap->firstBlock) == first && first >= base + sizeof(rwResHeap),
            what + ": the first block");
    std::uintptr_t at = first;
    const rwResHeapBlockHeader* prev = nullptr;
    bool prevFree = false, seenFirstFree = heap->firstFree == nullptr, freeBeforeHint = false;
    RwUInt32 largestFree = 0;
    for (const rwResHeapBlockHeader* b = heap->firstBlock; b != nullptr; b = b->next) {
        require(reinterpret_cast<std::uintptr_t>(b) == at && b->prev == prev && b->heap == heap && b->size % 32u == 0 &&
                    (b->flags & ~rwRESHEAP_BLOCKUSED) == 0,
                what + ": a block out of place");
        const bool free = (b->flags & rwRESHEAP_BLOCKUSED) == 0;
        require(!(free && prevFree), what + ": two free blocks side by side");
        if (b == heap->firstFree) seenFirstFree = true;
        if (free && !seenFirstFree) freeBeforeHint = true;
        if (free) largestFree = std::max(largestFree, b->size);
        prevFree = free;
        prev = b;
        at += sizeof(rwResHeapBlockHeader) + b->size;
    }
    require(at == end, what + ": the blocks do not tile the heap");
    require(seenFirstFree && !freeBeforeHint, what + ": a free block before firstFree");
    return largestFree;
}

// ---- the heap at every start offset, every size ----------------------------------------------------------------
void heapLayouts() {
    std::vector<char> area(70000);
    char* const aligned = reinterpret_cast<char*>((reinterpret_cast<std::uintptr_t>(area.data()) + 31) & ~std::uintptr_t{31});
    for (unsigned k = 0; k < 32; ++k) {
        for (std::size_t size : {std::size_t{0}, std::size_t{63}, std::size_t{95}, std::size_t{96}, std::size_t{97},
                                 std::size_t{127}, std::size_t{128}, std::size_t{129}, std::size_t{1000}, std::size_t{65536}}) {
            void* const memory = aligned + k;
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(memory);
            const std::uintptr_t first = (base + sizeof(rwResHeap) + 31) & ~std::uintptr_t{31};
            const std::uintptr_t end = (base + size) & ~std::uintptr_t{31};
            const bool fits = end >= first + 64;
            check(_rwResHeapInit(memory, size) == (fits ? 1 : 0), "init at offset " + std::to_string(k) + " size " + std::to_string(size));
            if (!fits) continue;
            check(checkHeap(memory, size, "init") == end - first - 32, "one free block of the rest");
            // fill it: every allocation 32-byte aligned and inside, until it is full
            std::vector<void*> got;
            for (void* p; (p = _rwResHeapAlloc(memory, 40)) != nullptr;) {
                check(reinterpret_cast<std::uintptr_t>(p) % 32 == 0 && reinterpret_cast<std::uintptr_t>(p) + 40 <= end,
                      "an allocation inside the heap");
                got.push_back(p);
            }
            checkHeap(memory, size, "full");
            for (std::size_t i = 0; i < got.size(); i += 2) _rwResHeapFree(got[i]);
            checkHeap(memory, size, "half freed");
            for (std::size_t i = 1; i < got.size(); i += 2) _rwResHeapFree(got[i]);
            check(checkHeap(memory, size, "freed") == end - first - 32, "all merged back into one block");
        }
    }
    // sizes past RwUInt32: refused, nothing written
    std::vector<char> small(256, 0x5A);
    char* const s = reinterpret_cast<char*>((reinterpret_cast<std::uintptr_t>(small.data()) + 31) & ~std::uintptr_t{31});
    check(_rwResHeapInit(s, std::size_t{5} << 30) == 0 &&
              std::all_of(small.begin(), small.end(), [](char c) { return c == 0x5A; }),
          "a heap past 4 GiB refused without writing");
    check(_rwResHeapInit(s, 192) == 1, "a small heap");
    check(_rwResHeapAlloc(s, std::numeric_limits<std::size_t>::max()) == nullptr &&
              _rwResHeapAlloc(s, std::size_t{0xFFFFFFF0u}) == nullptr && _rwResHeapAlloc(s, std::size_t{1} << 40) == nullptr,
          "sizes whose rounding would wrap are refused");
    checkHeap(s, 192, "after refusals");
    check(_rwResHeapClose(s) == 1, "close");
}

// ---- the engine's life -------------------------------------------------------------------------------------------
void engineLife() {
    // an arena of 64 KB: opened at RwEngineStart with the module's hint
    startEngine(65536);
    rwResources& res = rwResourcesArena();
    check(resourcesModule.numInstances == 1 && res.maxSize == 65536 && res.memHeap != nullptr && res.currentSize == 0 &&
              res.reusageSize == 0 && res.usedEntries == &res.entriesA && res.freeEntries == &res.entriesB,
          "the arena opened at RwEngineStart");
    const auto it = g_live.find(res.memHeap);
    check(it != g_live.end() && it->second.size == 65536 && it->second.hint == (rwMEMHINTDUR_GLOBAL | rwID_RESOURCESMODULE),
          "the arena's memory request");
    // entries, then the end of a frame through RwCameraShowRaster
    RwResEntry* refs[3] = {};
    for (int i = 0; i < 3; ++i) {
        check(RwResourcesAllocateResEntry(reinterpret_cast<void*>(std::uintptr_t(0x100 + i)), &refs[i], 100 * (i + 1), notify) == refs[i] &&
                  refs[i] != nullptr,
              "an entry");
    }
    check(res.usedEntries->link.next == &refs[2]->link && res.currentSize == 600, "used, most recent first");
    RwCamera* const camera = RwCameraCreate();
    require(camera != nullptr, "a camera");
    RwCameraShowRaster(camera, nullptr, 0);
    check(res.freeEntries->link.next == &refs[2]->link && res.usedEntries->link.next == &res.usedEntries->link,
          "RwCameraShowRaster purges the arena");
    RwResourcesUseResEntry(refs[0]);
    check(res.usedEntries->link.next == &refs[0]->link && res.freeEntries->link.next == &refs[2]->link &&
              res.freeEntries->link.prev == &refs[1]->link,
          "a used entry back on the used list");
    RwCameraDestroy(camera);
    // RwEngineStop empties the arena: every destroy callback, owner pointers cleared, memory freed
    g_notified.clear();
    require(RwEngineStop() == 1, "stop");
    check(g_notified.size() == 3 && refs[0] == nullptr && refs[1] == nullptr && refs[2] == nullptr && resourcesModule.numInstances == 0,
          "RwEngineStop frees every entry");
    require(RwEngineClose() == 1 && RwEngineTerm() == 1, "close / term");
    check(g_live.empty(), "nothing left");

    // the arena's allocation failing: the start fails with E_RW_NOMEM, nothing leaked; a later start succeeds
    {
        RwEngineOpenParams params{nullptr};
        require(RwEngineInit(&kFuncs, 0, 4096) == 1 && RwEngineOpen(&params) == 1, "init / open");
        bool failedOnce = false;
        for (long at = 0;; ++at) {
            const long before = g_mallocs;
            const std::size_t liveBefore = g_live.size();
            g_failAt = before + at;
            const RwBool started = RwEngineStart();
            g_failAt = -1;
            if (started) {
                check(resourcesModule.numInstances == 1 && rwResourcesArena().memHeap != nullptr, "started after failures");
                break;
            }
            failedOnce = true;
            check(g_live.size() == liveBefore && resourcesModule.numInstances == 0, "a failed start leaks nothing");
            takeError();
        }
        check(failedOnce, "the start failed at least once");
        stopEngine();
    }
    // an arena too small for a heap: the start fails, nothing leaked; the open itself reports
    // E_RW_INSUFFICIENTRESOURCES (0) and leaves memHeap cleared (a failed start closes the error module, losing it)
    {
        RwEngineOpenParams params{nullptr};
        require(RwEngineInit(&kFuncs, 0, 40) == 1 && RwEngineOpen(&params) == 1, "init / open");
        const std::size_t liveBefore = g_live.size();
        check(RwEngineStart() == 0 && resourcesModule.numInstances == 0 && g_live.size() == liveBefore,
              "a start with too small an arena fails");
        require(RwEngineClose() == 1 && RwEngineTerm() == 1, "close / term");
        check(g_live.empty(), "nothing left");
        startEngine(4096);
        void* const instance = RwEngineInstance;
        const RwInt32 offset = resourcesModule.globalsOffset;
        _rwResourcesClose(instance, offset, sizeof(rwResourcesGlobals));
        for (RwUInt32 size : {1u, 40u, 63u}) {   // too small at any alignment (95 fits on memory 16 past a 32-byte boundary)
            rwSrcGlobals().resArenaInitSize = size;
            takeError();
            const std::size_t live = g_live.size();
            check(_rwResourcesOpen(instance, offset, sizeof(rwResourcesGlobals)) == nullptr && g_live.size() == live &&
                      resourcesModule.numInstances == 0 && rwResourcesArena().memHeap == nullptr,
                  "an arena of " + std::to_string(size) + " bytes fails to open, memHeap cleared");
            check(isError(takeError(), E_RW_INSUFFICIENTRESOURCES), "its error");
        }
        rwSrcGlobals().resArenaInitSize = 96;   // the smallest arena with a heap (32-byte aligned memory: one block of 32)
        RwResEntry* e = nullptr;
        if (_rwResourcesOpen(instance, offset, sizeof(rwResourcesGlobals)) == instance) {
            const bool aligned = reinterpret_cast<std::uintptr_t>(rwResourcesArena().memHeap) % 32 == 0;
            check(!aligned || (RwResourcesAllocateResEntry(nullptr, &e, 0, nullptr) == nullptr && e == nullptr),
                  "no room in a 96-byte arena for a 48-byte entry header");
            takeError();
        } else {
            check(isError(takeError(), E_RW_INSUFFICIENTRESOURCES), "a 96-byte arena on 16-byte aligned memory");
            rwSrcGlobals().resArenaInitSize = 4096;
            require(_rwResourcesOpen(instance, offset, sizeof(rwResourcesGlobals)) == instance, "reopened");
        }
        stopEngine();
    }
    // a purge with no module open does nothing (RwCameraShowRaster may run outside a started engine)
    _rwResourcesPurge();
    check(resourcesModule.numInstances == 0, "a purge with no module open");
}

// ---- the deliberate differences -------------------------------------------------------------------------------------
void contracts() {
    startEngine(8192);
    rwResources& res = rwResourcesArena();
    RwResEntry* a = nullptr;
    RwResEntry* b = reinterpret_cast<RwResEntry*>(std::uintptr_t{1});
    check(RwResourcesAllocateResEntry(nullptr, &a, 500, notify) == a && a != nullptr, "an entry");
    takeError();
    g_notified.clear();
    for (RwInt32 size : {-1, -24, -25, -48, -1000, std::numeric_limits<RwInt32>::min()}) {
        b = reinterpret_cast<RwResEntry*>(std::uintptr_t{1});
        check(RwResourcesAllocateResEntry(nullptr, &b, size, notify) == nullptr && b == nullptr && g_notified.empty() &&
                  res.currentSize == 500 && res.usedEntries->link.next == &a->link,
              "a negative size refused at once: " + std::to_string(size));
        const RwError e = takeError();
        check(isError(e, E_RW_INSUFFICIENTRESOURCES), "its error");
    }
    // the largest size: every entry evicted first, as the original does, then the error
    RwResEntry* const first = a;
    check(RwResourcesAllocateResEntry(nullptr, &b, std::numeric_limits<RwInt32>::max(), nullptr) == nullptr && b == nullptr &&
              g_notified.size() == 1 && g_notified[0] == first && a == nullptr && res.currentSize == 0 && res.reusageSize == 500,
          "RwInt32's largest size evicts everything and fails");
    check(isError(takeError(), E_RW_INSUFFICIENTRESOURCES), "its error");
    stopEngine();

    // an arena of size 0: no memory, allocations refused at once
    startEngine(0);
    check(resourcesModule.numInstances == 1 && rwResourcesArena().memHeap == nullptr && rwResourcesArena().maxSize == 0,
          "an arena of size 0 opens without memory");
    b = reinterpret_cast<RwResEntry*>(std::uintptr_t{1});
    check(RwResourcesAllocateResEntry(nullptr, &b, 16, nullptr) == nullptr && b == nullptr &&
              isError(takeError(), E_RW_INSUFFICIENTRESOURCES),
          "no allocation from an arena of size 0");
    _rwResourcesPurge();
    check(RwResourcesEmptyArena() == 1, "purge / empty an arena of size 0");
    // an entry outside the arena (link.next NULL) is freed with RwFree
    auto* const outside = static_cast<RwResEntry*>(RwMalloc(sizeof(RwResEntry) + 64, 0));
    RwResEntry* ref = outside;
    *outside = RwResEntry{{nullptr, nullptr}, 64, nullptr, &ref, notify};
    g_notified.clear();
    check(RwResourcesUseResEntry(outside) == outside && outside->link.next == nullptr, "using an outside entry changes nothing");
    check(RwResourcesFreeResEntry(outside) == 1 && ref == nullptr && g_notified.size() == 1 && g_live.count(outside) == 0,
          "an outside entry freed with RwFree");
    stopEngine();
}

// ---- random sessions: the invariants at any size ---------------------------------------------------------------
struct Rng {
    std::uint64_t s = 0x2026110220261102ull;
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<std::uint32_t>(s >> 16);
    }
};

void randomSessions() {
    Rng rng;
    unsigned allocated = 0, failed = 0, evicting = 0;
    for (unsigned session = 0; session < 60; ++session) {
        const RwUInt32 arena = 128u + rng.next() % 32768u;
        startEngine(arena);
        rwResources& res = rwResourcesArena();
        const std::size_t heapBytes = g_live.at(res.memHeap).size;
        std::vector<RwResEntry*> slots(600, nullptr);   // each allocation's own owner pointer
        std::map<RwResEntry*, unsigned char> fill;   // each entry's data byte
        for (unsigned op = 0; op < 600; ++op) {
            const std::string what = "session " + std::to_string(session) + " op " + std::to_string(op);
            const std::uint32_t kind = rng.next() % 10u;
            if (kind < 6) {
                const RwInt32 size = static_cast<RwInt32>(rng.next() % 5u == 0 ? rng.next() % (arena + arena / 4u) : rng.next() % 300u);
                const std::size_t slot = op;
                const std::size_t entriesBefore = fill.size();
                g_notified.clear();
                RwResEntry* const e = RwResourcesAllocateResEntry(nullptr, &slots[slot], size, notify);
                for (RwResEntry* gone : g_notified) fill.erase(gone);
                if (e != nullptr) {
                    check(slots[slot] == e && e->size == size && reinterpret_cast<std::uintptr_t>(e + 1) % 16 == 0,
                          what + ": the new entry");
                    const auto byte = static_cast<unsigned char>(rng.next());
                    std::memset(e + 1, byte, static_cast<std::size_t>(size));
                    fill[e] = byte;
                    ++allocated;
                    if (fill.size() <= entriesBefore) ++evicting;
                } else {
                    check(fill.empty() && res.currentSize == 0 && slots[slot] == nullptr &&
                              isError(takeError(), E_RW_INSUFFICIENTRESOURCES),
                          what + ": a failed allocation leaves the arena empty");
                    check(checkHeap(res.memHeap, heapBytes, what) < ((static_cast<std::size_t>(size) + sizeof(RwResEntry) + 31) & ~std::size_t{31}),
                          what + ": it failed only because no block holds it");
                    ++failed;
                }
            } else if (kind < 7 && !fill.empty()) {
                auto it = fill.begin();
                std::advance(it, static_cast<long>(rng.next() % fill.size()));
                g_notified.clear();
                RwResourcesFreeResEntry(it->first);
                check(g_notified.size() == 1 && g_notified[0] == it->first, what + ": its destroy callback");
                fill.erase(it);
            } else if (kind < 9 && !fill.empty()) {
                auto it = fill.begin();
                std::advance(it, static_cast<long>(rng.next() % fill.size()));
                RwResourcesUseResEntry(it->first);
                check(res.usedEntries->link.next == &it->first->link, what + ": used");
            } else {
                _rwResourcesPurge();
                check(res.usedEntries->link.next == &res.usedEntries->link && res.reusageSize == 0, what + ": purged");
            }
            // the invariants
            checkHeap(res.memHeap, heapBytes, what);
            RwInt32 total = 0;
            std::size_t listed = 0;
            for (RwLinkList* list : {res.freeEntries, res.usedEntries}) {
                for (RwLLLink* l = list->link.next; l != &list->link; l = l->next) {
                    auto* const e = reinterpret_cast<RwResEntry*>(l);
                    const auto* const block = reinterpret_cast<const rwResHeapBlockHeader*>(
                        reinterpret_cast<const char*>(e) - sizeof(rwResHeapBlockHeader));
                    require(fill.count(e) == 1 && (block->flags & rwRESHEAP_BLOCKUSED) &&
                                block->size >= sizeof(RwResEntry) + static_cast<std::size_t>(e->size) && *e->ownerRef == e,
                            what + ": a listed entry");
                    const auto* data = reinterpret_cast<const unsigned char*>(e + 1);
                    require(std::all_of(data, data + e->size, [&](unsigned char c) { return c == fill[e]; }),
                            what + ": an entry's data changed");
                    total += e->size;
                    ++listed;
                }
            }
            check(listed == fill.size() && total == res.currentSize, what + ": the lists hold every entry");
        }
        stopEngine();
    }
    std::printf("  random sessions: %u entries (%u by evicting), %u failed allocations\n", allocated, evicting, failed);
    require(allocated > 15000 && evicting > 2000 && failed > 300, "the random sessions must reach every case");
}

}  // namespace

int main() {
    try {
        heapLayouts();
        engineLife();
        contracts();
        randomSessions();
        std::printf("rwcore_resources_native_tests: %u checks passed\n", g_checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_resources_native_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
