// Oracle for the native RenderWare growable arrays (rwcore/src/list.cpp): the ORIGINAL balist functions of PAL
// main.dol in the reference interpreter (rwcore_oracle.h) and the native ones run the same random operations; after
// each, the oracle compares every list's counts and entry size, every filled entry's bytes (both sides write the same
// bytes into new entries), the entry each call returns (as an index), the held error, and every RwMalloc / RwRealloc /
// RwFree (sizes and hints; the header is 16 bytes on the GameCube, sizeof(RwSList) natively).
// liveness-roots: reference-interpreter-only
//
// Usage: rwcore_list_tests <main.dol>
#include "rw/core/error.h"
#include "rw/core/list.h"
#include "rwcore_oracle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
using namespace rworacle;

constexpr Original kCreate{"_rwSListCreate", 0x8047ECE0u, 0x120u, 0x9421FFD0u, 0x4E800020u};
constexpr Original kEmpty{"_rwSListEmpty", 0x8047EE00u, 0xCu, 0x38000000u, 0x4E800020u};
constexpr Original kNewEntry{"_rwSListGetNewEntry", 0x8047EE0Cu, 0xE4u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kNewEntries{"_rwSListGetNewEntries", 0x8047EEF0u, 0xFCu, 0x9421FFE0u, 0x4E800020u};
constexpr Original kDestroyEnd{"_rwSListDestroyEndEntries", 0x8047EFECu, 0x10u, 0x80030004u, 0x4E800020u};
constexpr Original kGetArray{"_rwSListGetArray", 0x8047EFFCu, 0x1Cu, 0x80030004u, 0x4E800020u};
constexpr Original kDestroy{"_rwSListDestroy", 0x8047F018u, 0x74u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kNum{"_rwSListGetNumEntries", 0x8047F08Cu, 0x8u, 0x80630004u, 0x4E800020u};
constexpr Original kToArray{"_rwSListToArray", 0x8047F094u, 0x88u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kGetEntry{"_rwSListGetEntry", 0x8047F11Cu, 0x14u, 0x8003000Cu, 0x4E800020u};
constexpr Original kBegin{"_rwSListGetBegin", 0x8047F130u, 0x8u, 0x80630000u, 0x4E800020u};
constexpr Original kEnd{"_rwSListGetEnd", 0x8047F138u, 0x18u, 0x8083000Cu, 0x4E800020u};
constexpr Original kErrorOpen{"_rwErrorOpen", 0x8047EB88u, 0x38u, 0x390D6100u, 0x4E800020u};
constexpr u32 kGuestErrorOffset = 0x200u;

struct Call {
    std::size_t size;
    RwUInt32 hint;
};
std::vector<Call> g_mallocs, g_reallocs;
std::size_t g_frees = 0;
int g_fail_malloc = -1, g_fail_realloc = -1;
void* recMalloc(std::size_t size, RwUInt32 hint) {
    g_mallocs.push_back({size, hint});
    return static_cast<int>(g_mallocs.size()) - 1 == g_fail_malloc ? nullptr : std::malloc(size);
}
void* recRealloc(void* p, std::size_t size, RwUInt32 hint) {
    g_reallocs.push_back({size, hint});
    return static_cast<int>(g_reallocs.size()) - 1 == g_fail_realloc ? nullptr : std::realloc(p, size);
}
void recFree(void* p) {
    ++g_frees;
    std::free(p);
}

struct Pair {
    u32 guest;
    RwSList* native;
};

void same(Guest& g, const Pair& p, const char* what) {
    GuestMemory& m = g.memory();
    const u32 elements = m.peek32(p.guest), filled = m.peek32(p.guest + 4), alloced = m.peek32(p.guest + 8);
    const u32 size = m.peek32(p.guest + 12);
    require(static_cast<RwInt32>(filled) == p.native->numElementsFilled &&
                static_cast<RwInt32>(alloced) == p.native->numElementsAlloced &&
                static_cast<RwInt32>(size) == p.native->entrySize && (elements == 0) == (p.native->listElements == nullptr),
            what);
    for (u32 i = 0; i < filled * size; ++i) require(m.peek8(elements + i) == p.native->listElements[i], what);
}

void same_error(Guest& g) {
    const RwError& held = rwModuleGlobals<RwError>(errorModule);
    require(g.memory().peek32(kEngine + kGuestErrorOffset) == static_cast<u32>(held.pluginID) &&
                g.memory().peek32(kEngine + kGuestErrorOffset + 4) == static_cast<u32>(held.errorCode),
            "the held error differs from the original's");
}

long long guest_index(Guest& g, const Pair& p, u32 entry) {
    if (entry == 0) return -1;
    const u32 elements = g.memory().peek32(p.guest), size = g.memory().peek32(p.guest + 12);
    return (static_cast<long long>(entry) - elements) / size;
}

long long native_index(const Pair& p, void* entry) {
    if (entry == nullptr) return -1;
    return (static_cast<RwUInt8*>(entry) - p.native->listElements) / p.native->entrySize;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rwcore_list_tests main.dol");
        Guest g(argv[1]);
        for (const Original& f : {kCreate, kEmpty, kNewEntry, kNewEntries, kDestroyEnd, kGetArray, kDestroy, kNum,
                                  kToArray, kGetEntry, kBegin, kEnd, kErrorOpen}) {
            g.check_pinned(f);
        }
        std::vector<std::max_align_t> block(64 + sizeof(RwGlobals) / sizeof(std::max_align_t));
        RwEngineInstance = block.data();
        rwSrcGlobals() = RwGlobals{};
        rwSrcGlobals().memoryFuncs = rwHostMemoryFunctions();
        rwSrcGlobals().memoryFuncs.rwmalloc = recMalloc;
        rwSrcGlobals().memoryFuncs.rwrealloc = recRealloc;
        rwSrcGlobals().memoryFuncs.rwfree = recFree;
        const RwInt32 kNativeErrorOffset = static_cast<RwInt32>((sizeof(RwGlobals) + 15) & ~std::size_t{15});
        g.memory().write32(kErrorModule, 0);
        g.memory().write32(kErrorModule + 4, 0);
        g.call(kErrorOpen.entry, {kEngine, kGuestErrorOffset, 8});
        _rwErrorOpen(RwEngineInstance, kNativeErrorOffset, 8);

        Rng rng;
        u64 instructions = 0;
        unsigned checks = 0;
        std::vector<Pair> lists;
        std::size_t gm = 0, gr = 0, gfree = 0, nm = 0, nr = 0, nfree = 0;
        const auto calls = [&](const char* what) {
            require(g.mallocs.size() - gm == g_mallocs.size() - nm && g.reallocs.size() - gr == g_reallocs.size() - nr &&
                        g.frees.size() - gfree == g_frees - nfree,
                    what);
            for (std::size_t i = 0; i < g_mallocs.size() - nm; ++i) {
                const auto& a = g.mallocs[gm + i];
                const auto& b = g_mallocs[nm + i];
                require(a.hint == b.hint && (a.size == b.size || (a.size == 0x10 && b.size == sizeof(RwSList))), what);
            }
            for (std::size_t i = 0; i < g_reallocs.size() - nr; ++i) {
                require(g.reallocs[gr + i].size == g_reallocs[nr + i].size &&
                            g.reallocs[gr + i].hint == g_reallocs[nr + i].hint,
                        what);
            }
            gm = g.mallocs.size(), gr = g.reallocs.size(), gfree = g.frees.size();
            nm = g_mallocs.size(), nr = g_reallocs.size(), nfree = g_frees;
        };
        const auto fill = [&](const Pair& p, u32 gentry, void* nentry, RwInt32 count) {
            for (RwInt32 i = 0; i < count * p.native->entrySize; ++i) {
                const u8 b = static_cast<u8>(rng.next());
                g.memory().write8(gentry + static_cast<u32>(i), b);
                static_cast<u8*>(nentry)[i] = b;
            }
        };

        for (unsigned step = 0; step < 30000; ++step) {
            u32 op = rng.next() % 100u;
            if (lists.size() > 12) op = 97;
            if (op < 5 || lists.size() < 2) {                              // create
                const RwInt32 size = 1 + static_cast<RwInt32>(rng.next() % 64u);
                const RwUInt32 hint = rng.next() & 0x00FFFFFFu;
                if (rng.next() % 10u == 0) {                               // fail the header or the entries
                    const int which = static_cast<int>(rng.next() & 1u);
                    g.fail_call = static_cast<int>(g.mallocs.size()) + which;
                    g_fail_malloc = static_cast<int>(g_mallocs.size()) + which;
                }
                instructions += g.call(kCreate.entry, {static_cast<u32>(size), hint});
                RwSList* n = _rwSListCreate(size, hint);
                g.fail_call = -1, g_fail_malloc = -1;
                require((g.r3() == 0) == (n == nullptr), "_rwSListCreate succeeded on one side only");
                if (n) lists.push_back({g.r3(), n}), same(g, lists.back(), "a created list differs");
                calls("_rwSListCreate's calls differ");
            } else if (op < 45) {                                          // new entry
                Pair& p = lists[rng.next() % lists.size()];
                const RwUInt32 hint = rng.next() & 0x00FFFFFFu;
                if (rng.next() % 50u == 0) g.fail_realloc = static_cast<int>(g.reallocs.size()), g_fail_realloc = static_cast<int>(g_reallocs.size());
                instructions += g.call(kNewEntry.entry, {p.guest, hint});
                void* e = _rwSListGetNewEntry(p.native, hint);
                g.fail_realloc = -1, g_fail_realloc = -1;
                require(guest_index(g, p, g.r3()) == native_index(p, e), "_rwSListGetNewEntry returned another entry");
                if (e) fill(p, g.r3(), e, 1);
                same(g, p, "a list differs after _rwSListGetNewEntry");
                calls("_rwSListGetNewEntry's calls differ");
            } else if (op < 60) {                                          // new entries
                Pair& p = lists[rng.next() % lists.size()];
                const RwInt32 num = static_cast<RwInt32>(rng.next() % 40u);
                const RwUInt32 hint = rng.next() & 0x00FFFFFFu;
                if (rng.next() % 50u == 0) g.fail_realloc = static_cast<int>(g.reallocs.size()), g_fail_realloc = static_cast<int>(g_reallocs.size());
                instructions += g.call(kNewEntries.entry, {p.guest, static_cast<u32>(num), hint});
                void* e = _rwSListGetNewEntries(p.native, num, hint);
                g.fail_realloc = -1, g_fail_realloc = -1;
                require(guest_index(g, p, g.r3()) == native_index(p, e), "_rwSListGetNewEntries returned another entry");
                if (e) fill(p, g.r3(), e, num);
                same(g, p, "a list differs after _rwSListGetNewEntries");
                calls("_rwSListGetNewEntries' calls differ");
            } else if (op < 70) {                                          // drop end entries
                Pair& p = lists[rng.next() % lists.size()];
                const RwInt32 n = p.native->numElementsFilled ? static_cast<RwInt32>(rng.next() % (p.native->numElementsFilled + 1)) : 0;
                instructions += g.call(kDestroyEnd.entry, {p.guest, static_cast<u32>(n)});
                _rwSListDestroyEndEntries(p.native, n);
                same(g, p, "a list differs after _rwSListDestroyEndEntries");
            } else if (op < 72) {                                          // empty
                Pair& p = lists[rng.next() % lists.size()];
                instructions += g.call(kEmpty.entry, {p.guest});
                require(g.r3() == p.guest && _rwSListEmpty(p.native) == p.native, "_rwSListEmpty returns the list");
                same(g, p, "a list differs after _rwSListEmpty");
            } else if (op < 92) {                                          // queries
                Pair& p = lists[rng.next() % lists.size()];
                instructions += g.call(kNum.entry, {p.guest});
                require(static_cast<RwInt32>(g.r3()) == _rwSListGetNumEntries(p.native), "_rwSListGetNumEntries");
                instructions += g.call(kGetArray.entry, {p.guest});
                require((g.r3() == 0) == (_rwSListGetArray(p.native) == nullptr), "_rwSListGetArray");
                instructions += g.call(kBegin.entry, {p.guest});
                require((g.r3() == 0) == (_rwSListGetBegin(p.native) == nullptr), "_rwSListGetBegin");
                instructions += g.call(kEnd.entry, {p.guest});
                require(guest_index(g, p, g.r3()) == native_index(p, _rwSListGetEnd(p.native)), "_rwSListGetEnd");
                const RwInt32 k = static_cast<RwInt32>(rng.next() % 64u);
                instructions += g.call(kGetEntry.entry, {p.guest, static_cast<u32>(k)});
                require(guest_index(g, p, g.r3()) == native_index(p, _rwSListGetEntry(p.native, k)), "_rwSListGetEntry");
            } else {                                                       // destroy or to-array
                const std::size_t k = rng.next() % lists.size();
                const Pair p = lists[k];
                lists.erase(lists.begin() + static_cast<long>(k));
                if (rng.next() & 1u) {
                    instructions += g.call(kDestroy.entry, {p.guest});
                    require(g.r3() == 1 && _rwSListDestroy(p.native) == 1, "_rwSListDestroy returns TRUE");
                } else {
                    const u32 filled = g.memory().peek32(p.guest + 4), size = g.memory().peek32(p.guest + 12);
                    const u32 gelements = g.memory().peek32(p.guest);
                    instructions += g.call(kToArray.entry, {p.guest});
                    auto* array = static_cast<RwUInt8*>(_rwSListToArray(p.native));
                    require((g.r3() == 0) == (array == nullptr) && (g.r3() == 0 || g.r3() == gelements),
                            "_rwSListToArray's result");
                    for (u32 i = 0; array && i < filled * size; ++i) {
                        require(g.memory().peek8(g.r3() + i) == array[i], "_rwSListToArray's entries differ");
                    }
                    std::free(array);
                }
                calls("a destruction's calls differ");
            }
            same_error(g);
            ++checks;
        }
        std::printf("rwcore_list_tests: %u operations identical to the original (%llu original instructions)\n",
                    checks, static_cast<unsigned long long>(instructions));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_list_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
