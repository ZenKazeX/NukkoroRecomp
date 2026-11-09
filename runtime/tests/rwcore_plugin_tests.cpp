// Oracle for the native RenderWare plugin registry (rwcore/src/plugin.cpp): the ORIGINAL batkreg functions of PAL
// main.dol in the reference interpreter (rwcore_oracle.h) and the native ones run the same random sessions - open,
// plugins added to several registries, objects initialised / de-initialised / copied, offsets queried, close - with
// the engine's free-list allocators and with an application's own memoryAlloc / memoryFree. After every operation the
// oracle compares each registry's sizes and its entries in order (offset, size, plugin ID, which callbacks, the
// links), every result, the held error, every plugin callback the registry made (which one, offset, size, object)
// and the memoryAlloc / memoryFree / RwMalloc / RwFree calls (counts and hints: sizes differ with the pointer width).
//
// 64-bit: native plugin slices are 8-aligned where the GameCube's are 4-aligned (rw/core/plugin.h), so the original
// is given every size rounded up to 8 (object sizes too); both rules agree on such sizes, and the native must then
// produce the same offsets while keeping each plugin's own size. A separate check shows the native rounding.
// liveness-roots: reference-interpreter-only
//
// Usage: rwcore_plugin_tests <main.dol>
#include "rw/core/error.h"
#include "rw/core/memory.h"
#include "rw/core/plugin.h"
#include "rwcore_oracle.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

namespace {
using namespace rworacle;

constexpr Original kOpen{"_rwPluginRegistryOpen", 0x80482BE8u, 0x5Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kClose{"_rwPluginRegistryClose", 0x80482CA8u, 0x134u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kGetOffset{"_rwPluginRegistryGetPluginOffset", 0x80482DE8u, 0x30u, 0x80630010u, 0x4E800020u};
constexpr Original kAdd{"_rwPluginRegistryAddPlugin", 0x80482E18u, 0x2D4u, 0x9421FFC0u, 0x4E800020u};
constexpr Original kInit{"_rwPluginRegistryInitObject", 0x804830ECu, 0xA8u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kDeInit{"_rwPluginRegistryDeInitObject", 0x80483194u, 0x6Cu, 0x9421FFE0u, 0x4E800020u};
constexpr Original kCopy{"_rwPluginRegistryCopyObject", 0x80483200u, 0x7Cu, 0x9421FFE0u, 0x4E800020u};
constexpr Original kMemoryOpen{"_rwMemoryOpen", 0x804812C4u, 0x118u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kMemoryClose{"_rwMemoryClose", 0x804813DCu, 0x1A4u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kErrorOpen{"_rwErrorOpen", 0x8047EB88u, 0x38u, 0x390D6100u, 0x4E800020u};
constexpr Original kErrorGet{"RwErrorGet", 0x8047EC34u, 0x44u, 0x808D615Cu, 0x4E800020u};
constexpr u32 kFreeListAlloc = 0x80480E00u, kFreeListFree = 0x80480FA0u;   // _rwFreeListAllocReal / FreeReal
constexpr u32 kDefaultCtor = 0x80482DDCu, kDefaultDtor = 0x80482DE0u, kDefaultCopy = 0x80482DE4u;
constexpr u32 kNumOpen = 0x805F2658u;              // the open-engine count _rwGetNumEngineInstances reads
constexpr u32 kCtorStub = 0x817F0060u, kDtorStub = 0x817F0070u, kCopyStub = 0x817F0080u;
constexpr u32 kGuestErrorOffset = 0x200u;
constexpr u32 kRegs = kBuffer, kFuncs = kBuffer + 0x400u, kErr = kBuffer + 0x420u;
constexpr u32 kObjA = 0x81002000u, kObjB = 0x81003000u;   // objects (never dereferenced by the registry)
constexpr unsigned kMaxRegs = 6;

constexpr RwInt32 round8(RwInt32 v) { return (v + 7) & ~7; }

// ---- what each side's callbacks saw -------------------------------------------------------------------------------
struct Seen {
    int kind;          // 0 constructor, 1 destructor, 2 copy, 3 memoryAlloc, 4 memoryFree
    u32 a, b, c, d;    // objects / offset / size, as identities (see the recorders)
    bool operator==(const Seen&) const = default;
};
std::vector<Seen> g_native_seen;
std::deque<bool> g_native_fail;   // constructor results to come (true: return NULL)
unsigned char g_obj_a[16], g_obj_b[16];
u32 id_of(const void* p) { return p == g_obj_a ? kObjA : p == g_obj_b ? kObjB : p == nullptr ? 0u : 1u; }

void* nativeCtor(void* object, RwInt32 offset, RwInt32 size) {
    g_native_seen.push_back({0, id_of(object), 0, static_cast<u32>(offset), static_cast<u32>(round8(size))});
    const bool fail = !g_native_fail.empty() && g_native_fail.front();
    if (!g_native_fail.empty()) g_native_fail.pop_front();
    return fail ? nullptr : object;
}
void* nativeDtor(void* object, RwInt32 offset, RwInt32 size) {
    g_native_seen.push_back({1, id_of(object), 0, static_cast<u32>(offset), static_cast<u32>(round8(size))});
    return object;
}
void* nativeCopy(void* dst, const void* src, RwInt32 offset, RwInt32 size) {
    g_native_seen.push_back({2, id_of(dst), id_of(src), static_cast<u32>(offset), static_cast<u32>(round8(size))});
    return dst;
}

// an application's own memoryAlloc / memoryFree (the custom mode)
int g_native_alloc_calls = 0, g_fail_alloc = -1;
void* customAlloc(RwFreeList* fl, RwUInt32 hint) {
    g_native_seen.push_back({3, fl != nullptr, 0, hint, 0});
    return g_native_alloc_calls++ == g_fail_alloc ? nullptr : std::malloc(sizeof(RwPluginRegEntry));
}
RwFreeList* customFree(RwFreeList* fl, void* p) {
    g_native_seen.push_back({4, fl != nullptr, 0, 0, 0});
    std::free(p);
    return fl;
}

int g_malloc_calls = 0, g_fail_malloc = -1;
std::vector<RwUInt32> g_malloc_hints;
std::size_t g_frees = 0;
void* recMalloc(std::size_t size, RwUInt32 hint) {
    g_malloc_hints.push_back(hint);
    return g_malloc_calls++ == g_fail_malloc ? nullptr : std::malloc(size);
}
void recFree(void* p) {
    ++g_frees;
    std::free(p);
}

// ---- one registry on both sides ------------------------------------------------------------------------------------
struct Reg {
    u32 guest;
    RwPluginRegistry* native;
};

/// The registry's fields and its entries, compared through the rounding (the guest's sizes are the native's rounded).
void same(Guest& g, const Reg& r, const char* what) {
    GuestMemory& m = g.memory();
    const RwPluginRegistry& n = *r.native;
    require(m.peek32(r.guest) == static_cast<u32>(n.sizeOfStruct) &&
                m.peek32(r.guest + 4) == static_cast<u32>(n.origSizeOfStruct) &&
                m.peek32(r.guest + 8) == static_cast<u32>(n.maxSizeOfStruct) &&
                m.peek32(r.guest + 12) == static_cast<u32>(n.staticAlloc),
            what);
    u32 ge = m.peek32(r.guest + 0x10), gprev = 0;
    const RwPluginRegEntry* ne = n.firstRegEntry;
    const RwPluginRegEntry* nprev = nullptr;
    for (unsigned k = 0; ge != 0 || ne != nullptr; ++k) {
        require(ge != 0 && ne != nullptr && k < 1000, what);
        require(m.peek32(ge) == static_cast<u32>(ne->offset) && m.peek32(ge + 4) == static_cast<u32>(round8(ne->size)) &&
                    m.peek32(ge + 8) == ne->pluginID && m.peek32(ge + 0x38) == r.guest && ne->parentRegistry == r.native,
                what);
        for (u32 off = 0xC; off <= 0x1C; off += 4) require(m.peek32(ge + off) == 0, what);
        require(ne->readCB == nullptr && ne->writeCB == nullptr && ne->getSizeCB == nullptr && ne->alwaysCB == nullptr &&
                    ne->rightsCB == nullptr && ne->errStrCB == nullptr && m.peek32(ge + 0x2C) == 0,
                what);
        require((m.peek32(ge + 0x20) == kCtorStub) == (ne->constructCB == nativeCtor) &&
                    (m.peek32(ge + 0x20) == kDefaultCtor) == (ne->constructCB != nativeCtor) &&
                    (m.peek32(ge + 0x24) == kDtorStub) == (ne->destructCB == nativeDtor) &&
                    (m.peek32(ge + 0x24) == kDefaultDtor) == (ne->destructCB != nativeDtor) &&
                    (m.peek32(ge + 0x28) == kCopyStub) == (ne->copyCB == nativeCopy) &&
                    (m.peek32(ge + 0x28) == kDefaultCopy) == (ne->copyCB != nativeCopy),
                what);
        require(ne->constructCB == nativeCtor || ne->constructCB(g_obj_b, 0, 0) == g_obj_b, what);
        require(m.peek32(ge + 0x34) == gprev && ne->prevRegEntry == nprev, what);
        if (m.peek32(ge + 0x30) == 0) require(m.peek32(r.guest + 0x14) == ge && n.lastRegEntry == ne, what);
        gprev = ge, nprev = ne;
        ge = m.peek32(ge + 0x30), ne = ne->nextRegEntry;
    }
    require((m.peek32(r.guest + 0x14) == 0) == (n.lastRegEntry == nullptr), what);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rwcore_plugin_tests main.dol");
        Guest g(argv[1]);
        for (const Original& f : {kOpen, kClose, kGetOffset, kAdd, kInit, kDeInit, kCopy, kMemoryOpen, kMemoryClose,
                                  kErrorOpen, kErrorGet}) {
            g.check_pinned(f);
        }

        // the guest's recorders
        std::vector<Seen> guest_seen;
        std::deque<bool> guest_fail;
        int guest_alloc_calls = 0, guest_fail_alloc = -1;
        const auto obj_id = [](u32 p) { return p == kObjA || p == kObjB || p == 0 ? p : 1u; };
        g.stub(kCtorStub, [&](Cpu& c) {
            guest_seen.push_back({0, obj_id(c.gpr[3]), 0, c.gpr[4], c.gpr[5]});
            const bool fail = !guest_fail.empty() && guest_fail.front();
            if (!guest_fail.empty()) guest_fail.pop_front();
            if (fail) c.gpr[3] = 0;
        });
        g.stub(kDtorStub, [&](Cpu& c) { guest_seen.push_back({1, obj_id(c.gpr[3]), 0, c.gpr[4], c.gpr[5]}); });
        g.stub(kCopyStub, [&](Cpu& c) {
            guest_seen.push_back({2, obj_id(c.gpr[3]), obj_id(c.gpr[4]), c.gpr[5], c.gpr[6]});
        });
        g.stub(kMemoryAllocStub, [&](Cpu& c) {
            guest_seen.push_back({3, c.gpr[3] != 0, 0, c.gpr[4], 0});
            c.gpr[3] = guest_alloc_calls++ == guest_fail_alloc ? 0u : g.heap_alloc(0x3C);
        });
        g.stub(kMemoryFreeStub, [&](Cpu& c) {
            guest_seen.push_back({4, c.gpr[3] != 0, 0, 0, 0});
            g.heap_free(c.gpr[4]);
        });

        // the native engine: RwGlobals, then the error module's globals
        std::vector<std::max_align_t> block(64 + sizeof(RwGlobals) / sizeof(std::max_align_t));
        RwEngineInstance = block.data();
        rwSrcGlobals() = RwGlobals{};
        RwMemoryFunctions recording = rwHostMemoryFunctions();
        recording.rwmalloc = recMalloc;
        recording.rwfree = recFree;
        const RwInt32 nativeErrorOffset = static_cast<RwInt32>((sizeof(RwGlobals) + 15) & ~std::size_t{15});
        _rwErrorOpen(RwEngineInstance, nativeErrorOffset, 8);
        g.memory().write32(kErrorModule, 0), g.memory().write32(kErrorModule + 4, 0);
        g.call(kErrorOpen.entry, {kEngine, kGuestErrorOffset, 8});
        g.memory().write32(kFuncs, kMallocStub);
        g.memory().write32(kFuncs + 4, kFreeStub);
        g.memory().write32(kFuncs + 8, kReallocStub);
        g.memory().write32(kFuncs + 12, 0);

        Rng rng;
        u64 instructions = 0;
        unsigned checks = 0;
        std::size_t gm = 0, nm = 0, gf = 0, nf = 0;
        const auto same_calls = [&](const char* what) {
            require(g.mallocs.size() - gm == g_malloc_hints.size() - nm && g.frees.size() - gf == g_frees - nf, what);
            for (std::size_t i = 0; i < g.mallocs.size() - gm; ++i) require(g.mallocs[gm + i].hint == g_malloc_hints[nm + i], what);
            gm = g.mallocs.size(), nm = g_malloc_hints.size(), gf = g.frees.size(), nf = g_frees;
            require(guest_seen == g_native_seen, what);
            guest_seen.clear(), g_native_seen.clear();
        };
        const auto same_error = [&](const char* what) {
            instructions += g.call(kErrorGet.entry, {kErr});
            RwError e{};
            RwErrorGet(&e);
            require(g.memory().peek32(kErr) == static_cast<u32>(e.pluginID) &&
                        g.memory().peek32(kErr + 4) == static_cast<u32>(e.errorCode),
                    what);
        };

        std::vector<RwPluginRegistry> natives(kMaxRegs);
        for (unsigned session = 0; session < 400; ++session) {
            const bool custom = rng.next() % 2u == 0;
            instructions += g.call(kMemoryOpen.entry, {kFuncs});
            require(g.r3() == 1 && _rwMemoryOpen(&recording) == 1, "_rwMemoryOpen must succeed");
            g.memory().write32(kEngine + 0x118u, custom ? kMemoryAllocStub : kFreeListAlloc);
            g.memory().write32(kEngine + 0x11Cu, custom ? kMemoryFreeStub : kFreeListFree);
            rwSrcGlobals().memoryAlloc = custom ? customAlloc : _rwFreeListAllocReal;
            rwSrcGlobals().memoryFree = custom ? customFree : _rwFreeListFreeReal;
            same_calls("_rwMemoryOpen's calls differ");

            // registries: sizes rounded to 8 for both sides
            std::vector<Reg> regs;
            const unsigned numRegs = 1 + rng.next() % kMaxRegs;
            for (unsigned i = 0; i < numRegs; ++i) {
                const RwInt32 orig = round8(static_cast<RwInt32>(rng.next() % 400u));
                const RwInt32 max = rng.next() % 3u == 0 ? orig + static_cast<RwInt32>(rng.next() % 600u) : 0;
                natives[i] = rwPLUGINREGISTRYINIT(orig);
                natives[i].maxSizeOfStruct = max;
                const u32 gr = kRegs + i * 0x20u;
                g.memory().write32(gr, static_cast<u32>(orig));
                g.memory().write32(gr + 4, static_cast<u32>(orig));
                g.memory().write32(gr + 8, static_cast<u32>(max));
                for (u32 off = 12; off < 0x18; off += 4) g.memory().write32(gr + off, 0);
                regs.push_back({gr, &natives[i]});
            }

            if (session % 7 == 6) g.fail_call = static_cast<int>(g.mallocs.size()), g_fail_malloc = g_malloc_calls;
            instructions += g.call(kOpen.entry, {});
            const RwBool opened = _rwPluginRegistryOpen();
            g.fail_call = -1, g_fail_malloc = -1;
            require(g.r3() == static_cast<u32>(opened), "_rwPluginRegistryOpen's result");
            same_calls("_rwPluginRegistryOpen's calls differ");

            const unsigned ops = 20 + rng.next() % 120u;
            for (unsigned step = 0; step < ops; ++step) {
                const Reg& r = regs[rng.next() % regs.size()];
                const u32 op = rng.next() % 100u;
                if (op < 45) {                                                   // add a plugin
                    const RwInt32 size = static_cast<RwInt32>(rng.next() % 200u);
                    const RwUInt32 id = rng.next() % 24u;
                    const u32 pick = rng.next();
                    const bool ctor = pick & 1u, dtor = pick & 2u, copy = pick & 4u;
                    const u32 open = rng.next() % 16u == 0 ? 1u : 0u;
                    g.memory().write32(kNumOpen, open);
                    rwNumEngineInstances = static_cast<RwInt32>(open);
                    if (rng.next() % 12u == 0) {
                        if (custom) guest_fail_alloc = guest_alloc_calls, g_fail_alloc = g_native_alloc_calls;
                        else g.fail_call = static_cast<int>(g.mallocs.size()), g_fail_malloc = g_malloc_calls;
                    }
                    instructions += g.call(kAdd.entry, {r.guest, static_cast<u32>(round8(size)), id, ctor ? kCtorStub : 0u,
                                                        dtor ? kDtorStub : 0u, copy ? kCopyStub : 0u});
                    const RwInt32 offset = _rwPluginRegistryAddPlugin(r.native, size, id, ctor ? nativeCtor : nullptr,
                                                                      dtor ? nativeDtor : nullptr,
                                                                      copy ? nativeCopy : nullptr);
                    guest_fail_alloc = -1, g_fail_alloc = -1, g.fail_call = -1, g_fail_malloc = -1;
                    require(g.r3() == static_cast<u32>(offset), "_rwPluginRegistryAddPlugin's result");
                    same(g, r, "a registry differs after _rwPluginRegistryAddPlugin");
                    same_calls("_rwPluginRegistryAddPlugin's calls differ");
                    same_error("_rwPluginRegistryAddPlugin's error differs");
                } else if (op < 60) {                                            // query an offset
                    const RwUInt32 id = rng.next() % 26u;
                    instructions += g.call(kGetOffset.entry, {r.guest, id});
                    require(g.r3() == static_cast<u32>(_rwPluginRegistryGetPluginOffset(r.native, id)),
                            "_rwPluginRegistryGetPluginOffset's result");
                } else if (op < 80) {                                            // initialise an object
                    guest_fail.clear(), g_native_fail.clear();
                    for (unsigned k = 0; k < 40; ++k) {
                        const bool fail = rng.next() % 10u == 0;
                        guest_fail.push_back(fail), g_native_fail.push_back(fail);
                    }
                    instructions += g.call(kInit.entry, {r.guest, kObjA});
                    const RwPluginRegistry* result = _rwPluginRegistryInitObject(r.native, g_obj_a);
                    require((g.r3() == r.guest) == (result == r.native) && (g.r3() == 0) == (result == nullptr),
                            "_rwPluginRegistryInitObject's result");
                    same_calls("_rwPluginRegistryInitObject's callbacks differ");
                } else if (op < 90) {                                            // de-initialise
                    instructions += g.call(kDeInit.entry, {r.guest, kObjA});
                    require(g.r3() == r.guest && _rwPluginRegistryDeInitObject(r.native, g_obj_a) == r.native,
                            "_rwPluginRegistryDeInitObject's result");
                    same_calls("_rwPluginRegistryDeInitObject's callbacks differ");
                } else {                                                         // copy
                    instructions += g.call(kCopy.entry, {r.guest, kObjB, kObjA});
                    require(g.r3() == r.guest && _rwPluginRegistryCopyObject(r.native, g_obj_b, g_obj_a) == r.native,
                            "_rwPluginRegistryCopyObject's result");
                    same_calls("_rwPluginRegistryCopyObject's callbacks differ");
                }
                ++checks;
            }

            instructions += g.call(kClose.entry, {});
            require(g.r3() == 1 && _rwPluginRegistryClose() == 1, "_rwPluginRegistryClose returns TRUE");
            for (const Reg& r : regs) same(g, r, "a registry differs after _rwPluginRegistryClose");
            same_calls("_rwPluginRegistryClose's calls differ");
            instructions += g.call(kMemoryClose.entry, {});
            _rwMemoryClose();
            same_calls("_rwMemoryClose's calls differ");
            same_error("the error after a session differs");
        }

        // the native rounding: slices start at multiples of 8, each plugin keeping its own size
        require(_rwPluginRegistryOpen() == 1, "native reopen");
        rwSrcGlobals().memoryAlloc = customAlloc;
        rwSrcGlobals().memoryFree = customFree;
        rwNumEngineInstances = 0;
        RwPluginRegistry odd = rwPLUGINREGISTRYINIT(20);
        require(_rwPluginRegistryAddPlugin(&odd, 4, 1, nullptr, nullptr, nullptr) == 24 &&
                    _rwPluginRegistryAddPlugin(&odd, 9, 2, nullptr, nullptr, nullptr) == 32 &&
                    _rwPluginRegistryAddPlugin(&odd, 8, 3, nullptr, nullptr, nullptr) == 48 && odd.sizeOfStruct == 56 &&
                    odd.firstRegEntry->size == 4 && odd.firstRegEntry->nextRegEntry->size == 9,
                "the native plugin slices are not 8-aligned");
        _rwPluginRegistryClose();
        require(odd.sizeOfStruct == 20 && odd.firstRegEntry == nullptr, "close restores the registry");
        g_native_seen.clear();

        std::printf("rwcore_plugin_tests: %u operations identical to the original (%llu original instructions)\n",
                    checks, static_cast<unsigned long long>(instructions));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_plugin_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
