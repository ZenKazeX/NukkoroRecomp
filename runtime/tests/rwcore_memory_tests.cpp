// Oracle for the native RenderWare free lists (rwcore/src/memory.cpp): the ORIGINAL bamemory functions of PAL
// main.dol run in the reference interpreter (rwcore_oracle.h), the native ones on a native engine, the same random
// sequence of operations on both. Addresses differ (32-bit guest heap vs 64-bit host, a 8- vs 16-byte block link), so
// the oracle compares structure after every operation: each free list's fields, its blocks in list order with their
// bitmaps, every returned entry as (block, index), the master free list, the order of the list of all free lists, and
// every RwMalloc / RwFree (a block's size differs by exactly the link size, 8 bytes). Then bamatrix's open / close,
// which create and destroy the matrix free list: the list, the module globals and counters, the calls.
// liveness-roots: reference-interpreter-only
//
// Usage: rwcore_memory_tests <main.dol>
#include "rw/core/matrix.h"
#include "rw/core/memory.h"
#include "rwcore_oracle.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace {
using namespace rworacle;

constexpr Original kEnable{"_rwFreeListEnable", 0x80480A78u, 0x8u, 0x906D2A50u, 0x4E800020u};
constexpr Original kCreatePre{"RwFreeListCreateAndPreallocateSpace", 0x80480C54u, 0x20u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kCreate{"RwFreeListCreate", 0x80480C28u, 0x2Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kDestroy{"RwFreeListDestroy", 0x80480D2Cu, 0xD4u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kAlloc{"_rwFreeListAllocReal", 0x80480E00u, 0x1A0u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kFree{"_rwFreeListFreeReal", 0x80480FA0u, 0x18Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kForAll{"RwFreeListForAllUsed", 0x8048112Cu, 0x138u, 0x9421FFB0u, 0x4E800020u};
constexpr Original kOpen{"_rwMemoryOpen", 0x804812C4u, 0x118u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kClose{"_rwMemoryClose", 0x804813DCu, 0x1A4u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kInterface{"RwOsGetMemoryInterface", 0x80481580u, 0xCu, 0x806D615Cu, 0x4E800020u};
constexpr Original kMatrixOpen{"_rwMatrixOpen", 0x8047F8DCu, 0xD4u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kMatrixClose{"_rwMatrixClose", 0x8047F878u, 0x64u, 0x9421FFF0u, 0x4E800020u};
constexpr u32 kMatrixMultiplyScalar = 0x8047F150u;   // bamatrix's static MatrixMultiply

constexpr u32 kMaster = 0x805F2614u;       // _masterFreeListPtr
constexpr u32 kAllLists = 0x805F2618u;     // the list of all free lists
constexpr u32 kCallbackStub = 0x817F0040u;
constexpr u32 kGuestLinkSlack = 7, kNativeLinkSlack = sizeof(RwLLLink) - 1;

// ---- native memory functions that record their calls -----------------------------------------------------------
struct Call {
    std::size_t size;
    RwUInt32 hint;
};
std::vector<Call> g_mallocs;
std::vector<void*> g_frees;
int g_fail_call = -1;
void* recMalloc(std::size_t size, RwUInt32 hint) {
    g_mallocs.push_back({size, hint});
    if (static_cast<int>(g_mallocs.size()) - 1 == g_fail_call) return nullptr;
    return std::malloc(size);
}
void recFree(void* p) {
    g_frees.push_back(p);
    std::free(p);
}
std::vector<void*> g_visited;
void recordVisit(void* entry, void* data) {
    g_visited.push_back(entry);
    (void)data;
}

// ---- views: one free list's structure, the same shape on both sides ---------------------------------------------
struct View {
    u32 entrySize = 0, perBlock = 0, heapSize = 0, alignment = 0, flags = 0;
    std::vector<std::vector<u8>> blocks;   // bitmaps, list order
    bool operator==(const View&) const = default;
};

View guest_view(Guest& g, u32 fl, std::vector<u32>* blocks = nullptr) {
    View v;
    GuestMemory& m = g.memory();
    v.entrySize = m.peek32(fl), v.perBlock = m.peek32(fl + 4), v.heapSize = m.peek32(fl + 8);
    v.alignment = m.peek32(fl + 12), v.flags = m.peek32(fl + 0x18);
    for (u32 b = m.peek32(fl + 0x10), n = 0; b != fl + 0x10 && n < 100000; b = m.peek32(b), ++n) {
        std::vector<u8> bits(v.heapSize);
        for (u32 i = 0; i < v.heapSize; ++i) bits[i] = m.peek8(b + 8 + i);
        v.blocks.push_back(bits);
        if (blocks) blocks->push_back(b);
    }
    return v;
}

View native_view(const RwFreeList* fl, std::vector<const void*>* blocks = nullptr) {
    View v{fl->entrySize, fl->entriesPerBlock, fl->heapSize, fl->alignment, fl->flags, {}};
    for (const RwLLLink* b = fl->blockList.link.next; b != &fl->blockList.link; b = b->next) {
        const auto* bits = reinterpret_cast<const u8*>(b) + sizeof(RwLLLink);
        v.blocks.emplace_back(bits, bits + fl->heapSize);
        if (blocks) blocks->push_back(b);
    }
    return v;
}

/// (block ordinal, entry index) of an entry, from each side's block list and entry rule.
std::pair<int, long long> guest_entry(Guest& g, u32 fl, u32 entry) {
    std::vector<u32> blocks;
    const View v = guest_view(g, fl, &blocks);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const u32 base = (blocks[i] + v.heapSize + kGuestLinkSlack + v.alignment) & ~(v.alignment - 1u);
        if (entry >= base && entry < base + v.perBlock * v.entrySize && (entry - base) % v.entrySize == 0) {
            return {static_cast<int>(i), (entry - base) / v.entrySize};
        }
    }
    return {-1, entry == 0 ? -1 : -2};
}

std::pair<int, long long> native_entry(const RwFreeList* fl, const void* entry) {
    std::vector<const void*> blocks;
    native_view(fl, &blocks);
    const auto e = reinterpret_cast<std::uintptr_t>(entry);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const std::uintptr_t base = (reinterpret_cast<std::uintptr_t>(blocks[i]) + fl->heapSize + kNativeLinkSlack +
                                     fl->alignment) & ~std::uintptr_t{fl->alignment - 1u};
        if (e >= base && e < base + std::uintptr_t{fl->entriesPerBlock} * fl->entrySize && (e - base) % fl->entrySize == 0) {
            return {static_cast<int>(i), static_cast<long long>((e - base) / fl->entrySize)};
        }
    }
    return {-1, entry == nullptr ? -1 : -2};
}

struct Pair {
    u32 guest;
    RwFreeList* native;
    std::vector<std::pair<u32, void*>> live;   // allocated entries
    std::vector<std::pair<u32, void*>> freed;  // recently freed entries (for repeated frees)
};

void same(Guest& g, const Pair& p, const char* what) {
    const View a = guest_view(g, p.guest), b = native_view(p.native);
    if (!(a == b)) {
        std::fprintf(stderr, "guest {%u %u %u %u %x, %zu blocks} native {%u %u %u %u %x, %zu blocks}\n", a.entrySize,
                     a.perBlock, a.heapSize, a.alignment, a.flags, a.blocks.size(), b.entrySize, b.perBlock,
                     b.heapSize, b.alignment, b.flags, b.blocks.size());
        require(false, what);
    }
}

/// The calls since the last check: same count, hints, and sizes (a block +8 bytes natively).
void same_calls(Guest& g, std::size_t& gm, std::size_t& nm, std::size_t& gf, std::size_t& nf, const char* what) {
    require(g.mallocs.size() - gm == g_mallocs.size() - nm, what);
    for (std::size_t i = 0; i < g_mallocs.size() - nm; ++i) {
        const auto& a = g.mallocs[gm + i];
        const auto& b = g_mallocs[nm + i];
        require(a.hint == b.hint && (b.size == a.size || b.size == std::size_t{a.size} + (kNativeLinkSlack - kGuestLinkSlack)),
                what);
    }
    require(g.frees.size() - gf == g_frees.size() - nf, what);
    gm = g.mallocs.size(), nm = g_mallocs.size(), gf = g.frees.size(), nf = g_frees.size();
}

/// The list of all free lists: the same members in the same order.
void same_all_lists(Guest& g, const std::map<u32, RwFreeList*>& map) {
    std::vector<RwFreeList*> guest_order, native_order;
    for (u32 l = g.memory().peek32(kAllLists), n = 0; l != kAllLists && n < 10000; l = g.memory().peek32(l), ++n) {
        const auto it = map.find(l - 0x1C);
        require(it != map.end(), "the original's list of free lists holds an unknown list");
        guest_order.push_back(it->second);
    }
    for (RwLLLink* l = _rwFreeListAll()->link.next; l != &_rwFreeListAll()->link; l = l->next) {
        native_order.push_back(reinterpret_cast<RwFreeList*>(reinterpret_cast<char*>(l) - offsetof(RwFreeList, link)));
    }
    require(guest_order == native_order, "the list of all free lists differs");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rwcore_memory_tests main.dol");
        Guest g(argv[1]);
        for (const Original& f : {kEnable, kCreatePre, kCreate, kDestroy, kAlloc, kFree, kForAll, kOpen, kClose,
                                  kInterface, kMatrixOpen, kMatrixClose}) {
            g.check_pinned(f);
        }
        std::vector<std::pair<u32, u32>> guest_visits;
        g.stub(kCallbackStub, [&](Cpu& c) { guest_visits.push_back({c.gpr[3], c.gpr[4]}); });

        // native engine: memory functions recorded, the free-list allocators installed as RwEngineInit does
        std::vector<std::max_align_t> block(64 + sizeof(RwGlobals) / sizeof(std::max_align_t));
        RwEngineInstance = block.data();
        rwSrcGlobals() = RwGlobals{};
        rwSrcGlobals().memoryAlloc = _rwFreeListAllocReal;
        rwSrcGlobals().memoryFree = _rwFreeListFreeReal;
        RwMemoryFunctions recording = rwHostMemoryFunctions();
        recording.rwmalloc = recMalloc;
        recording.rwfree = recFree;

        // guest engine: the same
        g.memory().write32(kEngine + 0x118u, kAlloc.entry);
        g.memory().write32(kEngine + 0x11Cu, kFree.entry);
        const u32 funcs = kBuffer;
        g.memory().write32(funcs, kMallocStub);
        g.memory().write32(funcs + 4, kFreeStub);
        g.memory().write32(funcs + 8, 0);
        g.memory().write32(funcs + 12, 0);

        Rng rng;
        u64 instructions = 0;
        unsigned checks = 0;
        std::size_t gm = 0, nm = 0, gf = 0, nf = 0;

        instructions += g.call(kOpen.entry, {funcs});
        require(g.r3() == 1 && _rwMemoryOpen(&recording) == 1, "_rwMemoryOpen must succeed");
        const u32 guest_master = g.memory().peek32(kMaster);
        Pair master{guest_master, _rwFreeListMaster(), {}};
        same(g, master, "the master free list differs");
        instructions += g.call(kInterface.entry, {});
        require(g.r3() == kEngine + 0x108u && RwOsGetMemoryInterface() == &rwSrcGlobals().memoryFuncs,
                "RwOsGetMemoryInterface");

        std::vector<Pair> lists;
        std::map<u32, RwFreeList*> map;
        std::vector<RwFreeList> in_place(64);
        std::size_t in_place_used = 0;
        for (unsigned step = 0; step < 40000; ++step) {
            u32 op = rng.next() % 100u;
            if (lists.size() > 20) op = 95;                               // bounded: destroy one
            if (op < 6 || lists.size() < 3) {                               // create
                const RwInt32 entrySize = 1 + static_cast<RwInt32>(rng.next() % 300u);
                const RwInt32 perBlock = 1 + static_cast<RwInt32>(rng.next() % 70u);
                const RwInt32 align = std::array<RwInt32, 6>{0, 4, 8, 16, 32, 64}[rng.next() % 6u];
                const RwInt32 prealloc = static_cast<RwInt32>(rng.next() % 4u);
                const RwUInt32 hint = rng.next();
                const bool place = in_place_used < in_place.size() && rng.next() % 5u == 0;
                const bool wrapper = !place && rng.next() % 4u == 0;
                if (rng.next() % 9u == 0) g_fail_call = static_cast<int>(g_mallocs.size()), g.fail_call = static_cast<int>(g.mallocs.size());
                u32 gspace = 0;
                RwFreeList* nspace = nullptr;
                if (place) {
                    gspace = g.heap_alloc(0x28);
                    nspace = &in_place[in_place_used++];
                }
                if (wrapper) {
                    instructions += g.call(kCreate.entry, {static_cast<u32>(entrySize), static_cast<u32>(perBlock),
                                                           static_cast<u32>(align), hint});
                } else {
                    instructions += g.call(kCreatePre.entry, {static_cast<u32>(entrySize), static_cast<u32>(perBlock),
                                                              static_cast<u32>(align), static_cast<u32>(prealloc),
                                                              gspace, hint});
                }
                RwFreeList* n = wrapper ? RwFreeListCreate(entrySize, perBlock, align, hint)
                                        : RwFreeListCreateAndPreallocateSpace(entrySize, perBlock, align, prealloc,
                                                                              nspace, hint);
                g_fail_call = -1, g.fail_call = -1;
                require((g.r3() == 0) == (n == nullptr), "free-list creation succeeded on one side only");
                require(!place || g.r3() == 0 || (g.r3() == gspace && n == nspace), "an in-place list is not the space");
                same_calls(g, gm, nm, gf, nf, "creation's RwMalloc / RwFree calls differ");
                if (n != nullptr) {
                    lists.push_back({g.r3(), n, {}});
                    map[g.r3()] = n;
                    same(g, lists.back(), "a created free list differs");
                }
                same(g, master, "the master free list differs after a creation");
            } else if (op < 52) {                                           // alloc
                Pair& p = lists[rng.next() % lists.size()];
                if (p.live.size() > 300) continue;
                const RwUInt32 hint = rng.next();
                if (rng.next() % 40u == 0) g_fail_call = static_cast<int>(g_mallocs.size()), g.fail_call = static_cast<int>(g.mallocs.size());
                instructions += g.call(kAlloc.entry, {p.guest, hint});
                void* e = _rwFreeListAllocReal(p.native, hint);
                g_fail_call = -1, g.fail_call = -1;
                require(guest_entry(g, p.guest, g.r3()) == native_entry(p.native, e), "an allocation returned another entry");
                if (e != nullptr) p.live.push_back({g.r3(), e});
                same(g, p, "a free list differs after an allocation");
                same_calls(g, gm, nm, gf, nf, "an allocation's RwMalloc calls differ");
            } else if (op < 90) {                                           // free
                Pair& p = lists[rng.next() % lists.size()];
                const bool again = !p.freed.empty() && rng.next() % 8u == 0;
                const auto refree = again ? p.freed[rng.next() % p.freed.size()] : std::pair<u32, void*>{0, nullptr};
                const bool live_again = std::find(p.live.begin(), p.live.end(), refree) != p.live.end();
                if (again && !live_again && guest_entry(g, p.guest, refree.first).first >= 0 &&
                    native_entry(p.native, refree.second).first >= 0) {
                    // an entry freed before, its block still there: the original clears a clear bit (and frees the
                    // block when nothing in it is used)
                    instructions += g.call(kFree.entry, {p.guest, refree.first});
                    require(g.r3() == p.guest && _rwFreeListFreeReal(p.native, refree.second) == p.native,
                            "a repeated free's result");
                } else if (p.live.empty() || rng.next() % 25u == 0) {      // a pointer in no block
                    instructions += g.call(kFree.entry, {p.guest, 0x81000000u});
                    int dummy = 0;
                    require(g.r3() == 0 && _rwFreeListFreeReal(p.native, &dummy) == nullptr,
                            "freeing a foreign pointer must return NULL");
                } else {
                    const std::size_t k = rng.next() % p.live.size();
                    const auto [ge, ne] = p.live[k];
                    p.live.erase(p.live.begin() + static_cast<long>(k));
                    p.freed.push_back({ge, ne});
                    if (p.freed.size() > 40) p.freed.erase(p.freed.begin());
                    instructions += g.call(kFree.entry, {p.guest, ge});
                    require(g.r3() == p.guest && _rwFreeListFreeReal(p.native, ne) == p.native, "a free's result");
                }
                // a block released by this free may come back at the same address as a new block: the entries
                // freed from it are then no longer "freed entries of a live block" - forget them
                if (g_frees.size() != nf) p.freed.clear();
                same(g, p, "a free list differs after a free");
                same_calls(g, gm, nm, gf, nf, "a free's RwFree calls differ");
            } else if (op < 95) {                                           // for all used
                Pair& p = lists[rng.next() % lists.size()];
                guest_visits.clear();
                g_visited.clear();
                const u32 data = rng.next();
                instructions += g.call(kForAll.entry, {p.guest, kCallbackStub, data});
                require(g.r3() == p.guest && RwFreeListForAllUsed(p.native, recordVisit, nullptr) == p.native,
                        "RwFreeListForAllUsed's result");
                require(guest_visits.size() == g_visited.size(), "RwFreeListForAllUsed visits another number of entries");
                for (std::size_t i = 0; i < g_visited.size(); ++i) {
                    require(guest_visits[i].second == data &&
                                guest_entry(g, p.guest, guest_visits[i].first) == native_entry(p.native, g_visited[i]),
                            "RwFreeListForAllUsed visits another entry");
                }
                same_calls(g, gm, nm, gf, nf, "RwFreeListForAllUsed's RwMalloc / RwFree calls differ");
            } else if (op < 98) {                                           // destroy
                const std::size_t k = rng.next() % lists.size();
                const Pair p = lists[k];
                lists.erase(lists.begin() + static_cast<long>(k));
                instructions += g.call(kDestroy.entry, {p.guest});
                require(g.r3() == 1 && RwFreeListDestroy(p.native) == 1, "RwFreeListDestroy returns TRUE");
                map.erase(p.guest);
                same(g, master, "the master free list differs after a destruction");
                same_calls(g, gm, nm, gf, nf, "a destruction's RwFree calls differ");
            } else {                                                        // preallocation on / off
                const u32 on = rng.next() & 1u;
                instructions += g.call(kEnable.entry, {on});
                _rwFreeListEnable(static_cast<RwBool>(on));
            }
            same_all_lists(g, map);
            ++checks;
        }
        // bamatrix's open / close create and destroy the matrix free list (in place, in the module's static header)
        constexpr u32 kGuestMatrixOffset = 0x240u;
        const RwInt32 nativeMatrixOffset = static_cast<RwInt32>((sizeof(RwGlobals) + 15) & ~std::size_t{15});
        g.memory().write32(kMatrixModule, 0), g.memory().write32(kMatrixModule + 4, 0);
        matrixModule = {};
        for (unsigned round = 0; round < 64; ++round) {
            const bool fail = round % 4 == 3;
            if (fail) g_fail_call = static_cast<int>(g_mallocs.size()), g.fail_call = static_cast<int>(g.mallocs.size());
            instructions += g.call(kMatrixOpen.entry, {kEngine, kGuestMatrixOffset, 0x18});
            void* opened = _rwMatrixOpen(RwEngineInstance, nativeMatrixOffset, 0x18);
            g_fail_call = -1, g.fail_call = -1;
            require((g.r3() == kEngine) == (opened == RwEngineInstance) && (g.r3() == 0) == (opened == nullptr),
                    "_rwMatrixOpen's result");
            same_calls(g, gm, nm, gf, nf, "_rwMatrixOpen's RwMalloc / RwFree calls differ");
            const u32 gm_globals = kEngine + kGuestMatrixOffset;
            const rwMatrixGlobals& nm_globals = rwModuleGlobals<rwMatrixGlobals>(matrixModule);
            require(g.memory().peek32(kMatrixModule) == kGuestMatrixOffset &&
                        g.memory().peek32(kMatrixModule + 4) == static_cast<u32>(matrixModule.numInstances),
                    "matrixModule differs after _rwMatrixOpen");
            const u32 gfl = g.memory().peek32(gm_globals);
            require((gfl == 0) == (nm_globals.matrixFreeList == nullptr), "the matrix free list exists on one side only");
            if (opened != nullptr) {
                require(gfl == 0x805E3B18u, "the original's matrix free list is not its static header");
                Pair p{gfl, nm_globals.matrixFreeList, {}};
                same(g, p, "the matrix free list differs");
                map[gfl] = nm_globals.matrixFreeList;
                require(g.memory().peek32(gm_globals + 4) == static_cast<u32>(nm_globals.matrixOptimizations) &&
                            g.memory().peek32(gm_globals + 8) == kMatrixMultiplyScalar &&
                            nm_globals.multMatrix == MatrixMultiply,
                        "the matrix optimizations / multiply differ");
                for (u32 i = 0; i < 3; ++i) {
                    const RwReal* t = &nm_globals.tolerance.Normal;
                    require(g.memory().peek32(gm_globals + 0xC + 4 * i) == std::bit_cast<u32>(t[i]),
                            "the matrix tolerances differ");
                }
            }
            same_all_lists(g, map);
            instructions += g.call(kMatrixClose.entry, {kEngine, kGuestMatrixOffset, 0x18});
            require(g.r3() == kEngine && _rwMatrixClose(RwEngineInstance, nativeMatrixOffset, 0x18) == RwEngineInstance,
                    "_rwMatrixClose's result");
            map.erase(gfl);
            require(g.memory().peek32(gm_globals) == 0 && nm_globals.matrixFreeList == nullptr &&
                        g.memory().peek32(kMatrixModule + 4) == static_cast<u32>(matrixModule.numInstances),
                    "_rwMatrixClose leaves the free list or the count");
            same_calls(g, gm, nm, gf, nf, "_rwMatrixClose's RwFree calls differ");
            same_all_lists(g, map);
            ++checks;
        }

        instructions += g.call(kClose.entry, {});
        _rwMemoryClose();
        same_calls(g, gm, nm, gf, nf, "_rwMemoryClose's RwFree calls differ");
        require(g.memory().peek32(kMaster) == 0 && _rwFreeListMaster() == nullptr, "_rwMemoryClose leaves a master");

        std::printf("rwcore_memory_tests: %u operations structurally identical to the original (%llu original "
                    "instructions)\n", checks, static_cast<unsigned long long>(instructions));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_memory_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
