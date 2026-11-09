// Oracle for the native RenderWare rasters, textures and texture dictionaries (rwcore/src/raster.cpp, texture.cpp):
// the ORIGINAL baraster / batextur functions of PAL main.dol in the reference interpreter (rwcore_oracle.h) and the
// native ones run the same random sessions, with the device's raster functions (create, destroy, lock, unlock, mip
// levels, texture set raster) and the texture read callback scripted alike on both sides, and a raster, a texture and
// a dictionary plugin: rasters created (every type / format flag, the device refusing now and then), destroyed,
// locked (level and mode passed to the device), unlocked, their level counts (with and without the mipmap format bit);
// textures created (with and without a raster), destroyed (references dropped one at a time), named and masked
// (names up to 40 characters: the 32-byte cut and its error), given rasters; dictionaries created, destroyed, filled,
// searched by name in every letter case, made current or not; RwTextureRead hitting the current dictionary, every
// dictionary, or falling to the read callback (which makes a texture or finds none); the for-all walks with callbacks
// that stop; the mipmapping switches. After every operation: every raster's fields, every texture's raster /
// dictionary identity, names, filter-addressing word and references, every dictionary's texture order, the module's
// dictionary order and current dictionary, results, callbacks in order, the held error.
// Not generated (deliberate differences, rwcore_texture_native_tests): destroying a dictionary that holds a texture
// still referenced elsewhere, failing plugin constructors, no raster / texture module.
// liveness-roots: reference-interpreter-only
//
// Usage: rwcore_texture_tests <main.dol>
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/raster.h"
#include "rw/core/texture.h"
#include "rwcore_oracle.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
using namespace rworacle;

constexpr Original kRasterOpen{"_rwRasterOpen", 0x8048B07Cu, 0x14Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRasterCreate{"RwRasterCreate", 0x8048AEC4u, 0xFCu, 0x9421FFE0u, 0x4E800020u};
constexpr Original kRasterDestroy{"RwRasterDestroy", 0x8048AC2Cu, 0x78u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRasterLock{"RwRasterLock", 0x8048AFC0u, 0x50u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRasterUnlock{"RwRasterUnlock", 0x8048AB90u, 0x48u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRasterLevels{"RwRasterGetNumLevels", 0x8048AD34u, 0x60u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRasterRegister{"RwRasterRegisterPlugin", 0x8048ACA4u, 0x40u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kTextureOpen{"_rwTextureOpen", 0x8048E52Cu, 0x2E4u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kTextureClose{"_rwTextureClose", 0x8048E350u, 0x1DCu, 0x9421FFE0u, 0x4E800020u};
constexpr Original kTextureCreate{"RwTextureCreate", 0x8048DD2Cu, 0xB0u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kTextureDestroy{"RwTextureDestroy", 0x8048DDDCu, 0xC8u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSetName{"RwTextureSetName", 0x8048DEA4u, 0xACu, 0x9421FFE0u, 0x4E800020u};
constexpr Original kSetMask{"RwTextureSetMaskName", 0x8048DF50u, 0xACu, 0x9421FFE0u, 0x4E800020u};
constexpr Original kSetRaster{"RwTextureSetRaster", 0x8048DA50u, 0x64u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kTextureRead{"RwTextureRead", 0x8048E180u, 0x150u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kTextureRegister{"RwTextureRegisterPlugin", 0x8048E2D0u, 0x40u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSetMip{"RwTextureSetMipmapping", 0x8048D9F8u, 0x18u, 0x808D617Cu, 0x4E800020u};
constexpr Original kGetMip{"RwTextureGetMipmapping", 0x8048DA10u, 0x14u, 0x806D617Cu, 0x4E800020u};
constexpr Original kSetAutoMip{"RwTextureSetAutoMipmapping", 0x8048DA24u, 0x18u, 0x808D617Cu, 0x4E800020u};
constexpr Original kGetAutoMip{"RwTextureGetAutoMipmapping", 0x8048DA3Cu, 0x14u, 0x806D617Cu, 0x4E800020u};
constexpr Original kDictCreate{"RwTexDictionaryCreate", 0x8048DAB4u, 0xD4u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kDictDestroy{"RwTexDictionaryDestroy", 0x8048DB88u, 0xA8u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kForAllTextures{"RwTexDictionaryForAllTextures", 0x8048DC30u, 0x74u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kForAllDicts{"RwTexDictionaryForAllTexDictionaries", 0x8048DCA4u, 0x88u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kAddTexture{"RwTexDictionaryAddTexture", 0x8048DFFCu, 0x50u, 0x80040004u, 0x4E800020u};
constexpr Original kFindNamed{"RwTexDictionaryFindNamedTexture", 0x8048E04Cu, 0xC8u, 0x81230008u, 0x4E800020u};
constexpr Original kSetCurrent{"RwTexDictionarySetCurrent", 0x8048E114u, 0x14u, 0x808D617Cu, 0x4E800020u};
constexpr Original kGetCurrent{"RwTexDictionaryGetCurrent", 0x8048E128u, 0x14u, 0x806D617Cu, 0x4E800020u};
constexpr Original kDefaultFind{"TextureDefaultFind", 0x8048D96Cu, 0x8Cu, 0x9421FFE0u, 0x4E800020u};
constexpr Original kStringOpen{"_rwStringOpen", 0x80483E70u, 0x140u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kMemoryOpen{"_rwMemoryOpen", 0x804812C4u, 0x118u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRegOpen{"_rwPluginRegistryOpen", 0x80482BE8u, 0x5Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kErrorOpen{"_rwErrorOpen", 0x8047EB88u, 0x38u, 0x390D6100u, 0x4E800020u};
constexpr Original kErrorGet{"RwErrorGet", 0x8047EC34u, 0x44u, 0x808D615Cu, 0x4E800020u};
constexpr u32 kFreeListAlloc = 0x80480E00u, kFreeListFree = 0x80480FA0u;
constexpr u32 kRasterModule = 0x805F2670u, kTextureModule = 0x805F267Cu, kDefaultDict = 0x805F2678u;
constexpr u32 kGuestErrorOffset = 0x200u, kGuestRasterOffset = 0x300u, kGuestTextureOffset = 0x380u;
constexpr u32 kTexGlobals = kEngine + kGuestTextureOffset;
// stubs: plugin constructors / destructors of rasters, textures, dictionaries; the device; the read and walk callbacks
constexpr u32 kRasterCtor = 0x817F0100u, kRasterDtor = 0x817F0110u, kTexCtor = 0x817F0120u, kTexDtor = 0x817F0130u,
              kDictCtor = 0x817F0140u, kDictDtor = 0x817F0150u, kDevCreate = 0x817F0160u, kDevDestroy = 0x817F0170u,
              kDevLock = 0x817F0180u, kDevUnlock = 0x817F0190u, kDevLevels = 0x817F01A0u, kDevSetRaster = 0x817F01B0u,
              kReadCB = 0x817F01C0u, kTexWalk = 0x817F01D0u, kDictWalk = 0x817F01E0u;
constexpr u32 kFuncs = kBuffer, kErr = kBuffer + 0x20u, kName = kBuffer + 0x100u, kMask = kBuffer + 0x140u;

struct Seen {
    int kind;   // 0..5 plugin ctor/dtor (raster, texture, dict), 6 device create, 7 destroy, 8 lock, 9 unlock,
                // 10 levels, 11 set raster, 12 read, 13 texture walk, 14 dict walk
    int a, b;
    bool operator==(const Seen&) const = default;
};
std::vector<Seen> g_native_seen;
std::vector<u32> g_script;
std::size_t g_native_next = 0;
u32 script(std::size_t& n) { return g_script[n++ % g_script.size()]; }
std::map<const void*, int> g_native_ids;
int nid(const void* p) {
    if (p == nullptr) return -10;
    const auto it = g_native_ids.find(p);
    return it == g_native_ids.end() ? -1 : it->second;
}
template <int K> void* nCtorDtor(void* o, RwInt32, RwInt32) {
    // a texture's destructor records the references it sees (the last one, restored for the destructors)
    g_native_seen.push_back({K, nid(o), K == 3 ? static_cast<RwTexture*>(o)->refCount : 0});
    return o;
}
RwBool nDevCreate(void*, void* r, RwInt32 flags) {
    g_native_seen.push_back({6, nid(r), flags});
    return script(g_native_next) % 7u != 0;
}
RwBool nDevDestroy(void*, void* r, RwInt32) { g_native_seen.push_back({7, nid(r), 0}); return 1; }
RwBool nDevLock(void* out, void* r, RwInt32 flags) {
    g_native_seen.push_back({8, nid(r), flags});
    const bool ok = script(g_native_next) % 4u != 0;
    *static_cast<RwUInt8**>(out) = ok ? reinterpret_cast<RwUInt8*>(static_cast<std::uintptr_t>(0x1000u + (flags & 0xFFFF))) : nullptr;
    return ok;
}
RwBool nDevUnlock(void*, void* r, RwInt32) { g_native_seen.push_back({9, nid(r), 0}); return 1; }
RwBool nDevLevels(void* out, void* r, RwInt32) {
    g_native_seen.push_back({10, nid(r), 0});
    const u32 s = script(g_native_next);
    *static_cast<RwInt32*>(out) = static_cast<RwInt32>(s % 12u);
    return s % 5u != 0;
}
RwBool nDevSetRaster(void* t, void* r, RwInt32) {
    g_native_seen.push_back({11, nid(t), nid(r)});
    const bool ok = script(g_native_next) % 4u != 0;
    if (ok) static_cast<RwTexture*>(t)->raster = static_cast<RwRaster*>(r);
    return ok;
}
// the read callback makes a texture of that name (or finds none): the test registers its identity afterwards
RwTexture* g_native_made = nullptr;
RwTexture* nRead(const RwChar* name, const RwChar*) {
    g_native_seen.push_back({12, 0, 0});
    if (script(g_native_next) % 3u == 0) return nullptr;
    g_native_made = RwTextureCreate(nullptr);
    if (g_native_made != nullptr) RwTextureSetName(g_native_made, name);
    return g_native_made;
}
RwTexture* nTexWalk(RwTexture* t, void*) {
    g_native_seen.push_back({13, nid(t), 0});
    return script(g_native_next) % 5u == 0 ? nullptr : t;
}
RwTexDictionary* nDictWalk(RwTexDictionary* d, void*) {
    g_native_seen.push_back({14, nid(d), 0});
    return script(g_native_next) % 5u == 0 ? nullptr : d;
}

std::string randomName(Rng& rng, const std::vector<std::string>& known) {
    if (!known.empty() && rng.next() % 3u != 0) {
        std::string n = known[rng.next() % known.size()];
        for (char& c : n) {   // the same name in another letter case
            if (rng.next() % 2u) c = static_cast<char>((c >= 'a' && c <= 'z') ? c - 32 : (c >= 'A' && c <= 'Z') ? c + 32 : c);
        }
        if (rng.next() % 9u == 0 && !n.empty()) n.pop_back();   // a prefix: no match
        return n;
    }
    const u32 length = rng.next() % 9u == 0 ? 30u + rng.next() % 11u : 1u + rng.next() % 12u;
    std::string n;
    for (u32 i = 0; i < length; ++i) n.push_back("abcxyzABCXYZ_09@[`{"[rng.next() % 19u]);
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rwcore_texture_tests main.dol");
        Guest g(argv[1]);
        for (const Original& f : {kRasterOpen, kRasterCreate, kRasterDestroy, kRasterLock, kRasterUnlock, kRasterLevels,
                                  kRasterRegister, kTextureOpen, kTextureClose, kTextureCreate, kTextureDestroy, kSetName,
                                  kSetMask, kSetRaster, kTextureRead, kTextureRegister, kSetMip, kGetMip, kSetAutoMip,
                                  kGetAutoMip, kDictCreate, kDictDestroy, kForAllTextures, kForAllDicts, kAddTexture,
                                  kFindNamed, kSetCurrent, kGetCurrent, kDefaultFind, kStringOpen, kMemoryOpen, kRegOpen,
                                  kErrorOpen, kErrorGet}) {
            g.check_pinned(f);
        }
        GuestMemory& m = g.memory();
        Rng rng;
        for (unsigned i = 0; i < 89; ++i) g_script.push_back(rng.next());
        std::map<u32, int> gids;
        const auto gid = [&](u32 a) {
            if (a == 0) return -10;
            const auto it = gids.find(a);
            return it == gids.end() ? -1 : it->second;
        };
        std::vector<Seen> guest_seen;
        std::size_t guest_next = 0;
        u32 guest_made = 0;
        for (int k = 0; k < 6; ++k) {
            g.stub(kRasterCtor + 0x10u * k, [&, k](Cpu& c) {
                guest_seen.push_back({k, gid(c.gpr[3]), k == 3 ? static_cast<int>(m.peek32(c.gpr[3] + 0x54)) : 0});
            });
        }
        g.stub(kDevCreate, [&](Cpu& c) {
            guest_seen.push_back({6, gid(c.gpr[4]), static_cast<int>(c.gpr[5])});
            c.gpr[3] = script(guest_next) % 7u != 0;
        });
        g.stub(kDevDestroy, [&](Cpu& c) { guest_seen.push_back({7, gid(c.gpr[4]), 0}); c.gpr[3] = 1; });
        g.stub(kDevLock, [&](Cpu& c) {
            const u32 flags = c.gpr[5];
            guest_seen.push_back({8, gid(c.gpr[4]), static_cast<int>(flags)});
            const bool ok = script(guest_next) % 4u != 0;
            m.write32(c.gpr[3], ok ? 0x1000u + (flags & 0xFFFFu) : 0u);
            c.gpr[3] = ok;
        });
        g.stub(kDevUnlock, [&](Cpu& c) { guest_seen.push_back({9, gid(c.gpr[4]), 0}); c.gpr[3] = 1; });
        g.stub(kDevLevels, [&](Cpu& c) {
            guest_seen.push_back({10, gid(c.gpr[4]), 0});
            const u32 s = script(guest_next);
            m.write32(c.gpr[3], s % 12u);
            c.gpr[3] = s % 5u != 0;
        });
        g.stub(kDevSetRaster, [&](Cpu& c) {
            guest_seen.push_back({11, gid(c.gpr[3]), gid(c.gpr[4])});
            const bool ok = script(guest_next) % 4u != 0;
            if (ok) m.write32(c.gpr[3], c.gpr[4]);
            c.gpr[3] = ok;
        });
        g.stub(kTexWalk, [&](Cpu& c) {
            guest_seen.push_back({13, gid(c.gpr[3]), 0});
            if (script(guest_next) % 5u == 0) c.gpr[3] = 0;
        });
        g.stub(kDictWalk, [&](Cpu& c) {
            guest_seen.push_back({14, gid(c.gpr[3]), 0});
            if (script(guest_next) % 5u == 0) c.gpr[3] = 0;
        });
        // the guest read callback: a nested call of the original RwTextureCreate / SetName on a fresh interpreter
        // would clobber this one's state, so it reports what to make and the test makes it after the read returns
        bool guest_read_wants = false;
        g.stub(kReadCB, [&](Cpu& c) {
            guest_seen.push_back({12, 0, 0});
            guest_read_wants = script(guest_next) % 3u != 0;
            c.gpr[3] = guest_read_wants ? guest_made : 0u;
        });

        // ---- the guest engine -----------------------------------------------------------------------------------
        g.reset_engine();
        for (u32 a : {kErrorModule, kErrorModule + 4u, kRasterModule, kRasterModule + 4u, kTextureModule, kTextureModule + 4u,
                      kDefaultDict}) {
            m.write32(a, 0);
        }
        m.write32(kFuncs, kMallocStub), m.write32(kFuncs + 4, kFreeStub), m.write32(kFuncs + 8, kReallocStub);
        m.write32(kFuncs + 12, 0);
        g.call(kMemoryOpen.entry, {kFuncs});
        m.write32(kEngine + 0x118u, kFreeListAlloc), m.write32(kEngine + 0x11Cu, kFreeListFree);
        g.call(kErrorOpen.entry, {kEngine, kGuestErrorOffset, 8});
        g.call(kRegOpen.entry, {});
        g.call(kStringOpen.entry, {});
        m.write32(0x805F2658u, 0);
        g.call(kRasterRegister.entry, {8, 0x7101, kRasterCtor, kRasterCtor + 0x10u, 0});
        g.call(kTextureRegister.entry, {8, 0x7102, kTexCtor, kTexDtor, 0});
        // the dictionaries' registry has no exported register call the game links; its plugins come through
        // _rwPluginRegistryAddPlugin on texDictTKList (0x8056F108)
        g.call(0x80482E18u, {0x8056F108u, 8, 0x7103, kDictCtor, kDictDtor, 0});
        for (u32 i = 0; i < 29; ++i) m.write32(kEngine + 0x48u + 4u * i, 0);
        m.write32(kEngine + 0x48u + 4u * 4u, kDevCreate), m.write32(kEngine + 0x48u + 4u * 5u, kDevDestroy);
        m.write32(kEngine + 0x48u + 4u * 8u, kDevSetRaster), m.write32(kEngine + 0x48u + 4u * 15u, kDevLock);
        m.write32(kEngine + 0x48u + 4u * 16u, kDevUnlock), m.write32(kEngine + 0x48u + 4u * 28u, kDevLevels);
        g.call(kRasterOpen.entry, {kEngine, kGuestRasterOffset, 0x64});
        require(g.r3() == kEngine, "guest rasters open");
        g.call(kTextureOpen.entry, {kEngine, kGuestTextureOffset, 0x34});
        require(g.r3() == kEngine, "guest textures open");
        m.write32(kTexGlobals + 0x14u, kReadCB);   // the read callback (the original's would read image files)

        // ---- the native engine ----------------------------------------------------------------------------------
        RwEngineOpenParams params{nullptr};
        require(RwEngineInit(nullptr, 0, 0) == 1, "native init");
        require(RwRasterRegisterPlugin(8, 0x7101, nCtorDtor<0>, nCtorDtor<1>, nullptr) >= 0 &&
                    RwTextureRegisterPlugin(8, 0x7102, nCtorDtor<2>, nCtorDtor<3>, nullptr) >= 0 &&
                    _rwPluginRegistryAddPlugin(&rwTexDictionaryRegistry(), 8, 0x7103, nCtorDtor<4>, nCtorDtor<5>, nullptr) >= 0,
                "native plugins");
        require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "native start");
        RwGlobals& G = rwSrcGlobals();
        G.stdFunc[4] = nDevCreate, G.stdFunc[5] = nDevDestroy, G.stdFunc[8] = nDevSetRaster;
        G.stdFunc[15] = nDevLock, G.stdFunc[16] = nDevUnlock, G.stdFunc[28] = nDevLevels;
        RwTextureSetReadCallBack(nRead);

        struct Pair {
            u32 g;
            void* n;
        };
        std::vector<Pair> rasters, textures, dicts;
        std::vector<std::string> names;
        int next_id = 0;
        u64 instructions = 0;
        unsigned checks = 0, reads = 0, readHits = 0, truncations = 0, lockedOk = 0;
        const auto add = [&](std::vector<Pair>& list, u32 ga, void* na) {
            const int id = next_id++;
            gids[ga] = id, g_native_ids[na] = id;
            list.push_back({ga, na});
        };
        const auto forget = [&](std::vector<Pair>& list, std::size_t k) {
            gids.erase(list[k].g), g_native_ids.erase(list[k].n);
            list.erase(list.begin() + static_cast<long>(k));
        };
        // the default dictionary is the module's own (made by the open): both sides' are identity 9999
        const u32 gDefault = m.peek32(kDefaultDict);
        gids[gDefault] = 9999, g_native_ids[RwTexDictionaryGetCurrent()] = 9999;
        dicts.push_back({gDefault, RwTexDictionaryGetCurrent()});
        const auto sameCallbacks = [&](const std::string& what) {
            if (guest_seen != g_native_seen) {
                std::string s = what + ": callbacks differ; original";
                for (const Seen& x : guest_seen) s += " " + std::to_string(x.kind) + "/" + std::to_string(x.a) + "/" + std::to_string(x.b);
                s += ", native";
                for (const Seen& x : g_native_seen) s += " " + std::to_string(x.kind) + "/" + std::to_string(x.a) + "/" + std::to_string(x.b);
                require(false, s);
            }
            guest_seen.clear(), g_native_seen.clear();
            instructions += g.call(kErrorGet.entry, {kErr});
            RwError e{};
            RwErrorGet(&e);
            char buf[96];
            std::snprintf(buf, sizeof buf, ": errors (original %08X/%08X, native %08X/%08X)", m.peek32(kErr), m.peek32(kErr + 4),
                          static_cast<u32>(e.pluginID), static_cast<u32>(e.errorCode));
            require(m.peek32(kErr) == static_cast<u32>(e.pluginID) && m.peek32(kErr + 4) == static_cast<u32>(e.errorCode), what + buf);
        };
        const auto sameAll = [&](const std::string& what) {
            for (const Pair& p : rasters) {
                const auto& n = *static_cast<RwRaster*>(p.n);
                require(gid(m.peek32(p.g)) == nid(n.parent) && m.peek32(p.g + 4) == static_cast<u32>(reinterpret_cast<std::uintptr_t>(n.cpPixels)) &&
                            m.peek32(p.g + 8) == 0 && n.palette == nullptr,
                        what + ": raster pointers");
                require(m.peek32(p.g + 0xC) == static_cast<u32>(n.width) && m.peek32(p.g + 0x10) == static_cast<u32>(n.height) &&
                            m.peek32(p.g + 0x14) == static_cast<u32>(n.depth),
                        what + ": raster size");
                require((m.peek32(p.g + 0x1C) >> 16) == static_cast<u16>(n.nOffsetX) && (m.peek32(p.g + 0x1C) & 0xFFFF) == static_cast<u16>(n.nOffsetY) &&
                            m.peek8(p.g + 0x21) == n.cFlags && m.peek8(p.g + 0x22) == n.privateFlags,
                        what + ": raster offsets / flags");
            }
            for (const Pair& p : textures) {
                const auto& n = *static_cast<RwTexture*>(p.n);
                require(gid(m.peek32(p.g)) == nid(n.raster) && gid(m.peek32(p.g + 4)) == nid(n.dict), what + ": texture raster / dict");
                // the name and the mask as strings: a new texture's bytes after the NUL are the original's leftover
                // memory (RwTextureCreate writes only name[0] and mask[0])
                for (u32 field = 0; field < 2; ++field) {
                    const RwChar* nf = field == 0 ? n.name : n.mask;
                    for (u32 i = 0; i < 32; ++i) {
                        const char gc = static_cast<char>(m.peek8(p.g + 0x10 + 32 * field + i));
                        if (gc != nf[i]) require(false, what + ": texture name / mask byte " + std::to_string(32 * field + i));
                        if (gc == 0) break;
                    }
                }
                require(m.peek32(p.g + 0x50) == n.filterAddressing && m.peek32(p.g + 0x54) == static_cast<u32>(n.refCount),
                        what + ": texture filter / references");
            }
            for (const Pair& p : dicts) {
                const auto& n = *static_cast<RwTexDictionary*>(p.n);
                std::vector<int> go, no;
                for (u32 l = m.peek32(p.g + 8); l != p.g + 8; l = m.peek32(l)) go.push_back(gid(l - 8));
                for (const RwLLLink* l = n.texturesInDict.link.next; l != &n.texturesInDict.link; l = l->next) {
                    no.push_back(nid(reinterpret_cast<const char*>(l) - offsetof(RwTexture, lInDictionary)));
                }
                require(go == no, what + ": dictionary texture order");
                require(m.peek8(p.g) == n.object.type, what + ": dictionary type");
            }
            std::vector<int> go, no;
            for (u32 l = m.peek32(kTexGlobals); l != kTexGlobals; l = m.peek32(l)) go.push_back(gid(l - 0x10));
            RwTexDictionaryForAllTexDictionaries([](RwTexDictionary* d, void* v) {
                static_cast<std::vector<int>*>(v)->push_back(nid(d));
                return d;
            }, &no);
            require(go == no, what + ": the module's dictionary order");
            require(gid(m.peek32(kTexGlobals + 0x10)) == nid(RwTexDictionaryGetCurrent()), what + ": current dictionary");
        };
        const auto writeString = [&](u32 at, const std::string& s) {
            for (std::size_t i = 0; i <= s.size(); ++i) m.write8(at + static_cast<u32>(i), i < s.size() ? static_cast<u8>(s[i]) : 0);
        };
        const auto inDictReferencedElsewhere = [&](const Pair& d) {
            const auto& n = *static_cast<RwTexDictionary*>(d.n);
            for (const RwLLLink* l = n.texturesInDict.link.next; l != &n.texturesInDict.link; l = l->next) {
                const auto* t = reinterpret_cast<const RwTexture*>(reinterpret_cast<const char*>(l) - offsetof(RwTexture, lInDictionary));
                if (t->refCount > 1) return true;
            }
            return false;
        };
        const auto dropTexture = [&](const void* n) {   // a texture freed by a destroy: forget it
            for (std::size_t k = 0; k < textures.size(); ++k) {
                if (textures[k].n == n) return forget(textures, k);
            }
        };

        for (unsigned step = 0; step < 20000; ++step) {
            const u32 op = rng.next() % 100u;
            std::string what = "op " + std::to_string(op) + " step " + std::to_string(step);
            if (op < 8 && rasters.size() < 24) {                                                  // raster create
                const u32 types[] = {0, 1, 2, 4, 5, 0x80};
                const u32 formats[] = {0, 0x100, 0x500, 0x600, 0x8500, 0x9500, 0x2000, 0xF000};
                const u32 flags = types[rng.next() % 6u] | formats[rng.next() % 8u];
                const u32 w = 1u + rng.next() % 600u, h = 1u + rng.next() % 600u, d = rng.next() % 3u * 16u;
                instructions += g.call(kRasterCreate.entry, {w, h, d, flags});
                const u32 gr = g.r3();
                // a new raster has no identity yet: the device's create and the constructors see it as unknown (-1) on
                // both sides; the records get its identity below
                RwRaster* nr = RwRasterCreate(static_cast<RwInt32>(w), static_cast<RwInt32>(h), static_cast<RwInt32>(d),
                                              static_cast<RwInt32>(flags));
                require((gr == 0) == (nr == nullptr), what + ": RwRasterCreate");
                if (nr != nullptr) {
                    const int id = next_id;
                    gids[gr] = id, g_native_ids[nr] = id;
                    for (Seen& s : guest_seen) if (s.kind == 0 || s.kind == 6) s.a = id;   // they ran on the new raster
                    for (Seen& s : g_native_seen) if (s.kind == 0 || s.kind == 6) s.a = id;
                    gids.erase(gr), g_native_ids.erase(nr);
                    add(rasters, gr, nr);
                }
            } else if (op < 12 && !rasters.empty()) {                                             // raster destroy
                const std::size_t k = rng.next() % rasters.size();
                bool used = false;
                for (const Pair& t : textures) used = used || static_cast<RwTexture*>(t.n)->raster == rasters[k].n;
                if (used) continue;   // a texture's raster: the texture's destroy frees it
                instructions += g.call(kRasterDestroy.entry, {rasters[k].g});
                require(g.r3() == 1 && RwRasterDestroy(static_cast<RwRaster*>(rasters[k].n)) == 1, what + ": RwRasterDestroy");
                sameCallbacks(what);
                forget(rasters, k);
            } else if (op < 18 && !rasters.empty()) {                                             // lock / unlock / levels
                const Pair& r = rasters[rng.next() % rasters.size()];
                const u32 kind = rng.next() % 3u;
                if (kind == 0) {
                    const u32 level = rng.next() % 12u, mode = 1u + rng.next() % 15u;
                    instructions += g.call(kRasterLock.entry, {r.g, level, mode});
                    RwUInt8* p = RwRasterLock(static_cast<RwRaster*>(r.n), static_cast<RwUInt8>(level), static_cast<RwInt32>(mode));
                    require(g.r3() == static_cast<u32>(reinterpret_cast<std::uintptr_t>(p)), what + ": RwRasterLock");
                    lockedOk += p != nullptr;
                } else if (kind == 1) {
                    instructions += g.call(kRasterUnlock.entry, {r.g});
                    require(g.r3() == r.g && RwRasterUnlock(static_cast<RwRaster*>(r.n)) == r.n, what + ": RwRasterUnlock");
                } else {
                    const u8 format = static_cast<u8>(rng.next());
                    m.write8(r.g + 0x23, format), static_cast<RwRaster*>(r.n)->cFormat = format;
                    instructions += g.call(kRasterLevels.entry, {r.g});
                    require(g.r3() == static_cast<u32>(RwRasterGetNumLevels(static_cast<RwRaster*>(r.n))), what + ": RwRasterGetNumLevels");
                }
            } else if (op < 28 && textures.size() < 40) {                                         // texture create
                const bool withRaster = !rasters.empty() && rng.next() % 2u;
                std::size_t k = withRaster ? rng.next() % rasters.size() : 0;
                if (withRaster) {   // a raster belongs to one texture at most
                    for (const Pair& t : textures) if (static_cast<RwTexture*>(t.n)->raster == rasters[k].n) { k = SIZE_MAX; break; }
                    if (k == SIZE_MAX) continue;
                }
                instructions += g.call(kTextureCreate.entry, {withRaster ? rasters[k].g : 0u});
                const u32 gt = g.r3();
                RwTexture* nt = RwTextureCreate(withRaster ? static_cast<RwRaster*>(rasters[k].n) : nullptr);
                require(gt != 0 && nt != nullptr, what + ": RwTextureCreate");
                for (Seen& s : guest_seen) if (s.kind == 2) s.a = next_id;
                for (Seen& s : g_native_seen) if (s.kind == 2) s.a = next_id;
                add(textures, gt, nt);
            } else if (op < 36 && !textures.empty()) {                                            // texture destroy
                const std::size_t k = rng.next() % textures.size();
                const Pair t = textures[k];
                const bool last = static_cast<RwTexture*>(t.n)->refCount == 1;
                RwRaster* raster = static_cast<RwTexture*>(t.n)->raster;
                instructions += g.call(kTextureDestroy.entry, {t.g});
                require(g.r3() == 1 && RwTextureDestroy(static_cast<RwTexture*>(t.n)) == 1, what + ": RwTextureDestroy");
                sameCallbacks(what);
                if (last) {
                    forget(textures, k);
                    for (std::size_t r = 0; r < rasters.size(); ++r) if (rasters[r].n == raster) { forget(rasters, r); break; }
                }
            } else if (op < 50 && !textures.empty()) {                                            // names
                const Pair& t = textures[rng.next() % textures.size()];
                const std::string n = randomName(rng, names);
                writeString(kName, n);
                const bool mask = rng.next() % 4u == 0;
                instructions += g.call(mask ? kSetMask.entry : kSetName.entry, {t.g, kName});
                RwTexture* r = mask ? RwTextureSetMaskName(static_cast<RwTexture*>(t.n), n.c_str())
                                    : RwTextureSetName(static_cast<RwTexture*>(t.n), n.c_str());
                require(g.r3() == t.g && r == t.n, what + ": RwTextureSetName / MaskName");
                truncations += n.size() >= 32;
                if (!mask) names.push_back(n.substr(0, 31));
            } else if (op < 54 && !textures.empty()) {                                            // set raster
                const Pair& t = textures[rng.next() % textures.size()];
                const bool clear = rasters.empty() || rng.next() % 3u == 0;
                const Pair* r = clear ? nullptr : &rasters[rng.next() % rasters.size()];
                if (r != nullptr) {
                    bool used = false;
                    for (const Pair& o : textures) used = used || static_cast<RwTexture*>(o.n)->raster == r->n;
                    if (used) continue;
                }
                instructions += g.call(kSetRaster.entry, {t.g, r != nullptr ? r->g : 0u});
                RwTexture* res = RwTextureSetRaster(static_cast<RwTexture*>(t.n), r != nullptr ? static_cast<RwRaster*>(r->n) : nullptr);
                require((g.r3() == t.g) == (res == t.n) && (g.r3() == 0) == (res == nullptr), what + ": RwTextureSetRaster");
            } else if (op < 58 && dicts.size() < 8) {                                             // dict create
                instructions += g.call(kDictCreate.entry, {});
                const u32 gd = g.r3();
                RwTexDictionary* nd = RwTexDictionaryCreate();
                require(gd != 0 && nd != nullptr, what + ": RwTexDictionaryCreate");
                for (Seen& s : guest_seen) if (s.kind == 4) s.a = next_id;
                for (Seen& s : g_native_seen) if (s.kind == 4) s.a = next_id;
                add(dicts, gd, nd);
            } else if (op < 61 && dicts.size() > 1) {                                             // dict destroy
                const std::size_t k = rng.next() % dicts.size();
                if (inDictReferencedElsewhere(dicts[k])) continue;   // the native difference
                std::vector<const void*> freed;
                const auto& nd = *static_cast<RwTexDictionary*>(dicts[k].n);
                for (const RwLLLink* l = nd.texturesInDict.link.next; l != &nd.texturesInDict.link; l = l->next) {
                    freed.push_back(reinterpret_cast<const char*>(l) - offsetof(RwTexture, lInDictionary));
                }
                std::vector<RwRaster*> freedRasters;
                for (const void* t : freed) freedRasters.push_back(static_cast<const RwTexture*>(t)->raster);
                instructions += g.call(kDictDestroy.entry, {dicts[k].g});
                require(g.r3() == 1 && RwTexDictionaryDestroy(static_cast<RwTexDictionary*>(dicts[k].n)) == 1, what + ": RwTexDictionaryDestroy");
                sameCallbacks(what);
                for (const void* t : freed) dropTexture(t);
                for (RwRaster* r : freedRasters) {
                    for (std::size_t q = 0; q < rasters.size(); ++q) if (rasters[q].n == r) { forget(rasters, q); break; }
                }
                forget(dicts, k);
            } else if (op < 70 && !textures.empty()) {                                            // add texture
                const Pair& t = textures[rng.next() % textures.size()];
                const Pair& d = dicts[rng.next() % dicts.size()];
                instructions += g.call(kAddTexture.entry, {d.g, t.g});
                require(g.r3() == t.g && RwTexDictionaryAddTexture(static_cast<RwTexDictionary*>(d.n), static_cast<RwTexture*>(t.n)) == t.n,
                        what + ": RwTexDictionaryAddTexture");
            } else if (op < 78) {                                                                 // find
                const Pair& d = dicts[rng.next() % dicts.size()];
                const std::string n = randomName(rng, names);
                writeString(kName, n);
                instructions += g.call(kFindNamed.entry, {d.g, kName});
                require(gid(g.r3()) == nid(RwTexDictionaryFindNamedTexture(static_cast<RwTexDictionary*>(d.n), n.c_str())),
                        what + ": RwTexDictionaryFindNamedTexture");
            } else if (op < 82) {                                                                 // current
                const bool none = rng.next() % 3u == 0;
                const Pair& d = dicts[rng.next() % dicts.size()];
                instructions += g.call(kSetCurrent.entry, {none ? 0u : d.g});
                RwTexDictionarySetCurrent(none ? nullptr : static_cast<RwTexDictionary*>(d.n));
            } else if (op < 92) {                                                                 // read
                std::string n = randomName(rng, names);
                if (!textures.empty() && rng.next() % 2u) {   // a live texture's name, in any letter case
                    n = static_cast<RwTexture*>(textures[rng.next() % textures.size()].n)->name;
                    for (char& c : n) if (rng.next() % 2u && c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
                }
                writeString(kName, n);
                const bool mask = rng.next() % 2u;
                writeString(kMask, "mask");
                // the texture the read callback would return, made first on both sides (unnamed, unregistered)
                sameCallbacks(what + " (before the read)");
                instructions += g.call(kTextureCreate.entry, {0});
                guest_made = g.r3();
                g_native_made = nullptr;
                instructions += g.call(kTextureRead.entry, {kName, mask ? kMask : 0u});
                const u32 gt = g.r3();
                // the native callback makes its texture inside the read; the guest's was made before: give the
                // guest's its name now (as the native callback named it)
                RwTexture* nt = RwTextureRead(n.c_str(), mask ? "mask" : nullptr);
                ++reads;
                const bool fresh = guest_read_wants && gt == guest_made;
                if (!(guest_read_wants && gt == guest_made)) {
                    instructions += g.call(kTextureDestroy.entry, {guest_made});   // the unused one
                }
                if (fresh) {
                    instructions += g.call(kSetName.entry, {gt, kName});
                    require(nt != nullptr && nt == g_native_made, what + ": a texture read by the callback");
                    for (Seen& s : guest_seen) if (s.kind == 2) s.a = next_id;
                    for (Seen& s : g_native_seen) if (s.kind == 2) s.a = next_id;
                    // the guest's constructor ran before the read: move it after the read's callback record, as the
                    // native one (made inside the callback) is recorded
                    std::stable_partition(guest_seen.begin(), guest_seen.end(), [](const Seen& s) { return s.kind == 12; });
                    std::stable_partition(g_native_seen.begin(), g_native_seen.end(), [](const Seen& s) { return s.kind == 12; });
                    add(textures, gt, nt);
                } else {
                    // only the guest made (and destroyed) a spare texture: its constructor / destructor records go
                    guest_seen.erase(std::remove_if(guest_seen.begin(), guest_seen.end(), [](const Seen& s) { return s.kind == 2 || s.kind == 3; }),
                                     guest_seen.end());
                    require(gid(gt) == nid(nt), what + ": RwTextureRead");
                    readHits += gt != 0;
                }
                guest_read_wants = false;
            } else if (op < 96) {                                                                 // walks
                if (rng.next() & 1u) {
                    const Pair& d = dicts[rng.next() % dicts.size()];
                    instructions += g.call(kForAllTextures.entry, {d.g, kTexWalk, 0});
                    require(g.r3() == d.g && RwTexDictionaryForAllTextures(static_cast<RwTexDictionary*>(d.n), nTexWalk, nullptr) == d.n,
                            what + ": RwTexDictionaryForAllTextures");
                } else {
                    instructions += g.call(kForAllDicts.entry, {kDictWalk, 0});
                    require(g.r3() == 1 && RwTexDictionaryForAllTexDictionaries(nDictWalk, nullptr) == 1,
                            what + ": RwTexDictionaryForAllTexDictionaries");
                }
            } else {                                                                              // mipmapping
                const u32 v = rng.next() % 3u;
                const bool automip = rng.next() & 1u;
                instructions += g.call(automip ? kSetAutoMip.entry : kSetMip.entry, {v});
                require(g.r3() == 1 && (automip ? RwTextureSetAutoMipmapping(static_cast<RwBool>(v)) : RwTextureSetMipmapping(static_cast<RwBool>(v))) == 1,
                        what + ": RwTextureSet(Auto)Mipmapping");
                instructions += g.call(automip ? kGetAutoMip.entry : kGetMip.entry, {});
                require(g.r3() == static_cast<u32>(automip ? RwTextureGetAutoMipmapping() : RwTextureGetMipmapping()),
                        what + ": RwTextureGet(Auto)Mipmapping");
            }
            sameCallbacks(what);
            sameAll(what);
            ++checks;
        }
        std::printf("  textures %zu, rasters %zu, dictionaries %zu alive; reads %u (hits %u), names cut %u, locks granted %u\n",
                    textures.size(), rasters.size(), dicts.size(), reads, readHits, truncations, lockedOk);
        require(reads > 1000 && readHits > 300 && truncations > 50 && lockedOk > 200, "the sessions must reach every case");
        std::printf("rwcore_texture_tests: %u operations identical to the original (%llu original instructions)\n", checks,
                    static_cast<unsigned long long>(instructions));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_texture_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
