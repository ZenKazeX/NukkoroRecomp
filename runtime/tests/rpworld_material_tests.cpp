// Oracle for the native RenderWare materials, material lists, texture stream reads and string chunks
// (rpworld/src/material.cpp, rwcore/src/texture.cpp RwTextureStreamRead, rwcore/src/string.cpp
// _rwStringStreamFindAndRead): the ORIGINAL bamateri / bamatlst / babintex / string functions of PAL main.dol in the
// reference interpreter (rwcore_oracle.h) and the native ones run the same random sessions with a material plugin and
// a pool of named textures in the current dictionary: materials created, destroyed (references dropped one at a
// time), given textures (or none); two material lists appended to (past the 20-entry growth), searched, read,
// emptied; materials read from streams - struct chunks of every length up to 28 bytes, bad versions, untextured and
// textured, the texture chunk's filter word (every filter, V addressing 0 or not, the automatic-mip bit), names of
// textures that exist (in any letter case) or not, masks, texture chunks of bad versions, empty extensions; texture
// chunks read alone (RwTextureStreamRead); string chunks read alone into a caller's buffer or a new one, behind other
// chunks. After every operation: every material's texture identity, colour bytes, pipeline, surface properties and
// references, every list's materials / count / space, every texture's references, filter word and dictionary, the
// mipmapping switches, results, the held error.
// Not generated (deliberate differences, rpworld_material_native_tests): structs longer than 28 bytes, texture structs
// longer than 4, names longer than 127 characters, failing plugin constructors or plugin reads, Unicode strings.
// liveness-roots: reference-interpreter-only
//
// Usage: rpworld_material_tests <main.dol>
#include "rw/core/binary.h"
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/stream.h"
#include "rw/core/texture.h"
#include "rw/world/material.h"
#include "rw/world/world.h"
#include "rwcore_oracle.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
using namespace rworacle;

constexpr Original kMatOpen{"_rpMaterialOpen", 0x8045C098u, 0x90u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kMatCreate{"RpMaterialCreate", 0x8045C18Cu, 0xB8u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kMatDestroy{"RpMaterialDestroy", 0x8045C244u, 0x7Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kMatSetTexture{"RpMaterialSetTexture", 0x8045C2C0u, 0x5Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kMatRegister{"RpMaterialRegisterPlugin", 0x8045C31Cu, 0x40u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kMatStreamRead{"RpMaterialStreamRead", 0x8045C398u, 0x3A8u, 0x9421FFB0u, 0x4E800020u};
constexpr Original kListDeinit{"_rpMaterialListDeinitialize", 0x8045C740u, 0x98u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kListInit{"_rpMaterialListInitialize", 0x8045C7D8u, 0x14u, 0x38000000u, 0x4E800020u};
constexpr Original kListGet{"_rpMaterialListGetMaterial", 0x8045C7ECu, 0x10u, 0x80630000u, 0x4E800020u};
constexpr Original kListAppend{"_rpMaterialListAppendMaterial", 0x8045C7FCu, 0x140u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kListFind{"_rpMaterialListFindMaterialIndex", 0x8045C93Cu, 0x38u, 0x80C30004u, 0x4E800020u};
constexpr Original kTexStreamRead{"RwTextureStreamRead", 0x80484D78u, 0x264u, 0x9421FEC0u, 0x4E800020u};
constexpr Original kStringRead{"_rwStringStreamFindAndRead", 0x804840B0u, 0x3E4u, 0x9421FEF0u, 0x4E800020u};
constexpr Original kTextureOpen{"_rwTextureOpen", 0x8048E52Cu, 0x2E4u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kTextureCreate{"RwTextureCreate", 0x8048DD2Cu, 0xB0u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSetName{"RwTextureSetName", 0x8048DEA4u, 0xACu, 0x9421FFE0u, 0x4E800020u};
constexpr Original kAddTexture{"RwTexDictionaryAddTexture", 0x8048DFFCu, 0x50u, 0x80040004u, 0x4E800020u};
constexpr Original kGetMip{"RwTextureGetMipmapping", 0x8048DA10u, 0x14u, 0x806D617Cu, 0x4E800020u};
constexpr Original kGetAutoMip{"RwTextureGetAutoMipmapping", 0x8048DA3Cu, 0x14u, 0x806D617Cu, 0x4E800020u};
constexpr Original kStringOpen{"_rwStringOpen", 0x80483E70u, 0x140u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kMemoryOpen{"_rwMemoryOpen", 0x804812C4u, 0x118u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRegOpen{"_rwPluginRegistryOpen", 0x80482BE8u, 0x5Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kErrorOpen{"_rwErrorOpen", 0x8047EB88u, 0x38u, 0x390D6100u, 0x4E800020u};
constexpr Original kErrorGet{"RwErrorGet", 0x8047EC34u, 0x44u, 0x808D615Cu, 0x4E800020u};
constexpr Original kStreamModuleOpen{"_rwStreamModuleOpen", 0x80481CBCu, 0x8Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kStreamOpen{"RwStreamOpen", 0x804823B8u, 0x288u, 0x9421FFC0u, 0x4E800020u};
constexpr Original kStreamClose{"RwStreamClose", 0x8048229Cu, 0x11Cu, 0x9421FFE0u, 0x4E800020u};
constexpr u32 kFreeListAlloc = 0x80480E00u, kFreeListFree = 0x80480FA0u;
constexpr u32 kStreamModule = 0x805F2628u, kTextureModule = 0x805F267Cu, kDefaultDict = 0x805F2678u,
              kMaterialModule = 0x805F2590u;
constexpr u32 kGuestErrorOffset = 0x200u, kGuestStreamOffset = 0x290u, kGuestTextureOffset = 0x380u,
              kGuestMaterialOffset = 0x3C0u;
constexpr u32 kTexGlobals = kEngine + kGuestTextureOffset;
constexpr u32 kCtorStub = 0x817F0100u, kDtorStub = 0x817F0110u, kReadCB = 0x817F0120u;
constexpr u32 kFuncs = kBuffer, kErr = kBuffer + 0x20u, kName = kBuffer + 0x100u, kMem = kBuffer + 0x200u,
              kLists = kBuffer + 0x300u, kStringBuf = kBuffer + 0x400u, kData = 0x81010000u;

struct Seen {
    int kind;   // 0 ctor, 1 dtor, 2 texture read callback (a = the mipmapping switches it sees: mip | auto << 1)
    int a;
    bool operator==(const Seen&) const = default;
};
std::vector<Seen> g_native_seen;
std::map<const void*, int> g_native_ids;
int nid(const void* p) {
    if (p == nullptr) return -10;
    const auto it = g_native_ids.find(p);
    return it == g_native_ids.end() ? -1 : it->second;
}
void* nCtor(void* o, RwInt32, RwInt32) { g_native_seen.push_back({0, nid(o)}); return o; }
// the texture read callback: loads a new texture now and then (the switches a stream read set are what it sees)
bool g_read_wants = false;
RwTexture* g_native_loaded = nullptr;
RwTexture* nRead(const RwChar* name, const RwChar*) {
    g_native_seen.push_back({2, (RwTextureGetMipmapping() ? 1 : 0) | (RwTextureGetAutoMipmapping() ? 2 : 0)});
    if (!g_read_wants) return nullptr;
    g_native_loaded = RwTextureCreate(nullptr);
    RwTextureSetName(g_native_loaded, name);
    return g_native_loaded;
}
void* nDtor(void* o, RwInt32, RwInt32) { g_native_seen.push_back({1, nid(o)}); return o; }

/// A little-endian RenderWare stream being written.
struct Bytes {
    std::vector<u8> b;
    void put(u32 w) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(w >> (8 * i))); }
    void putBytes(const void* p, std::size_t n) { b.insert(b.end(), static_cast<const u8*>(p), static_cast<const u8*>(p) + n); }
};
constexpr u32 libraryID(u32 v) { return (((v - 0x30000u) << 14) & 0xFFC00000u) | ((v & 0x3Fu) << 16) | 0x37u; }

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rpworld_material_tests main.dol");
        Guest g(argv[1]);
        for (const Original& f : {kMatOpen, kMatCreate, kMatDestroy, kMatSetTexture, kMatRegister, kMatStreamRead, kListDeinit,
                                  kListInit, kListGet, kListAppend, kListFind, kTexStreamRead, kStringRead, kTextureOpen,
                                  kTextureCreate, kSetName, kAddTexture, kGetMip, kGetAutoMip, kStringOpen, kMemoryOpen,
                                  kRegOpen, kErrorOpen, kErrorGet, kStreamModuleOpen, kStreamOpen, kStreamClose}) {
            g.check_pinned(f);
        }
        GuestMemory& m = g.memory();
        Rng rng;
        std::map<u32, int> gids;
        const auto gid = [&](u32 a) {
            if (a == 0) return -10;
            const auto it = gids.find(a);
            return it == gids.end() ? -1 : it->second;
        };
        std::vector<Seen> guest_seen;
        g.stub(kCtorStub, [&](Cpu& c) { guest_seen.push_back({0, gid(c.gpr[3])}); });
        g.stub(kDtorStub, [&](Cpu& c) { guest_seen.push_back({1, gid(c.gpr[3])}); });
        u32 guest_spare = 0;   // a texture made before a read, for the read callback to return
        bool guest_loaded = false;
        g.stub(kReadCB, [&](Cpu& c) {
            guest_seen.push_back({2, (m.peek32(kEngine + kGuestTextureOffset + 0x1Cu) ? 1 : 0) |
                                         (m.peek32(kEngine + kGuestTextureOffset + 0x20u) ? 2 : 0)});
            guest_loaded = g_read_wants;
            c.gpr[3] = g_read_wants ? guest_spare : 0u;
        });

        // ---- the guest engine -----------------------------------------------------------------------------------
        g.reset_engine();
        for (u32 a : {kErrorModule, kErrorModule + 4u, kStreamModule, kStreamModule + 4u, kTextureModule, kTextureModule + 4u,
                      kDefaultDict, kMaterialModule, kMaterialModule + 4u}) {
            m.write32(a, 0);
        }
        m.write32(kFuncs, kMallocStub), m.write32(kFuncs + 4, kFreeStub), m.write32(kFuncs + 8, kReallocStub);
        m.write32(kFuncs + 12, 0);
        g.call(kMemoryOpen.entry, {kFuncs});
        m.write32(kEngine + 0x118u, kFreeListAlloc), m.write32(kEngine + 0x11Cu, kFreeListFree);
        g.call(kErrorOpen.entry, {kEngine, kGuestErrorOffset, 8});
        g.call(kRegOpen.entry, {});
        g.call(kStringOpen.entry, {});
        g.call(kStreamModuleOpen.entry, {kEngine, kGuestStreamOffset, 4});
        m.write32(0x805F2658u, 0);
        g.call(kMatRegister.entry, {8, 0x7301, kCtorStub, kDtorStub, 0});
        g.call(kTextureOpen.entry, {kEngine, kGuestTextureOffset, 0x34});
        require(g.r3() == kEngine, "guest textures open");
        m.write32(kTexGlobals + 0x14u, kReadCB);
        g.call(kMatOpen.entry, {kEngine, kGuestMaterialOffset, 4});
        require(g.r3() == kEngine, "guest materials open");

        // ---- the native engine ----------------------------------------------------------------------------------
        RwEngineOpenParams params{nullptr};
        require(RwEngineInit(nullptr, 0, 0) == 1 && RpWorldPluginAttach() == 1, "native init");
        require(RpMaterialRegisterPlugin(8, 0x7301, nCtor, nDtor, nullptr) >= 0, "the native material plugin");
        require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "native start");
        RwTextureSetReadCallBack(nRead);

        // ---- a pool of named textures in the current (default) dictionary, alike on both sides ---------------------
        struct Pair {
            u32 g;
            void* n;
        };
        std::vector<Pair> textures, materials;
        std::vector<std::string> names;
        int next_id = 0;
        u64 instructions = 0;
        const u32 gDict = m.peek32(kDefaultDict);
        const auto writeString = [&](u32 at, const std::string& s) {
            for (std::size_t i = 0; i <= s.size(); ++i) m.write8(at + static_cast<u32>(i), i < s.size() ? static_cast<u8>(s[i]) : 0);
        };
        for (int i = 0; i < 16; ++i) {
            std::string n = "tex";
            for (int k = 0; k < 1 + i % 7; ++k) n.push_back("abcdefghijklmnopqrstuvwxyz"[rng.next() % 26u]);
            names.push_back(n);
            instructions += g.call(kTextureCreate.entry, {0});
            const u32 gt = g.r3();
            writeString(kName, n);
            g.call(kSetName.entry, {gt, kName});
            g.call(kAddTexture.entry, {gDict, gt});
            RwTexture* nt = RwTextureCreate(nullptr);
            RwTextureSetName(nt, n.c_str());
            RwTexDictionaryAddTexture(RwTexDictionaryGetCurrent(), nt);
            gids[gt] = next_id, g_native_ids[nt] = next_id;
            ++next_id;
            textures.push_back({gt, nt});
        }
        // two material lists: guest structs at kLists, kLists + 0x10
        RpMaterialList nativeLists[2];
        for (u32 l = 0; l < 2; ++l) {
            g.call(kListInit.entry, {kLists + 0x10u * l});
            _rpMaterialListInitialize(&nativeLists[l]);
        }

        unsigned checks = 0, read = 0, readTextured = 0, readFoundTexture = 0, appends = 0, strings = 0, loaded = 0;
        const auto sameCallbacks = [&](const std::string& what) {
            require(guest_seen == g_native_seen, what + ": callbacks");
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
            for (const Pair& p : materials) {
                const auto& n = *static_cast<RpMaterial*>(p.n);
                u32 colour = 0;
                std::memcpy(&colour, &n.color, 4);
                const u32 gColour = m.peek32(p.g + 4);
                const u32 nColourBE = (colour >> 24) | ((colour >> 8) & 0xFF00u) | ((colour << 8) & 0xFF0000u) | (colour << 24);
                require(gid(m.peek32(p.g)) == nid(n.texture) && gColour == nColourBE && m.peek32(p.g + 8) == 0 && n.pipeline == nullptr,
                        what + ": material texture / colour / pipeline");
                require(m.peek32(p.g + 0xC) == std::bit_cast<u32>(n.surfaceProps.ambient) &&
                            m.peek32(p.g + 0x10) == std::bit_cast<u32>(n.surfaceProps.specular) &&
                            m.peek32(p.g + 0x14) == std::bit_cast<u32>(n.surfaceProps.diffuse) &&
                            static_cast<RwInt16>(m.peek32(p.g + 0x18) >> 16) == n.refCount,
                        what + ": material surface / references");
            }
            for (u32 l = 0; l < 2; ++l) {
                const u32 gl = kLists + 0x10u * l;
                const RpMaterialList& nl = nativeLists[l];
                require(static_cast<RwInt32>(m.peek32(gl + 4)) == nl.numMaterials && static_cast<RwInt32>(m.peek32(gl + 8)) == nl.space,
                        what + ": list count / space");
                for (RwInt32 i = 0; i < nl.numMaterials; ++i) {
                    require(gid(m.peek32(m.peek32(gl) + 4u * static_cast<u32>(i))) == nid(nl.materials[i]), what + ": list entry");
                }
            }
            for (const Pair& p : textures) {
                const auto& n = *static_cast<RwTexture*>(p.n);
                require(m.peek32(p.g + 0x54) == static_cast<u32>(n.refCount) && m.peek32(p.g + 0x50) == n.filterAddressing &&
                            gid(m.peek32(p.g + 4)) == (n.dict == RwTexDictionaryGetCurrent() ? gid(gDict) : nid(n.dict)),
                        what + ": texture references / filter / dictionary");
            }
            instructions += g.call(kGetMip.entry, {});
            const u32 mip = g.r3();
            instructions += g.call(kGetAutoMip.entry, {});
            require(mip == static_cast<u32>(RwTextureGetMipmapping()) && g.r3() == static_cast<u32>(RwTextureGetAutoMipmapping()),
                    what + ": the mipmapping switches");
        };
        // stream pieces
        const auto stringChunk = [&](Bytes& s, const std::string& text, u32 version) {
            const u32 length = (static_cast<u32>(text.size()) + 4u) & ~3u;   // NUL included, padded to 4
            s.put(rwID_STRING), s.put(length), s.put(libraryID(version));
            std::vector<u8> chars(length, 0);
            std::memcpy(chars.data(), text.data(), text.size());
            s.putBytes(chars.data(), length);
        };
        const auto textureChunk = [&](Bytes& s, bool known) {
            const u32 filter = rng.next() % 7u, u = rng.next() % 5u, v = rng.next() % 3u == 0 ? 0u : rng.next() % 5u;
            const u32 word = filter | (u << 8) | (v << 12) | ((rng.next() & 1u) << 16) | ((rng.next() & 0xFFu) << 24);
            std::string name = known ? names[rng.next() % names.size()] : "missing" + std::to_string(rng.next() % 100000u);
            for (char& c : name) if (rng.next() % 3u == 0 && c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
            const u32 structLength = rng.next() % 8u == 0 ? 0u : 4u;
            s.put(rwID_TEXTURE), s.put(0), s.put(libraryID(0x36003u));
            s.put(rwID_STRUCT), s.put(structLength), s.put(libraryID(rng.next() % 12u == 0 ? 0x34000u : 0x36003u));
            if (structLength) s.put(word);
            if (rng.next() % 6u == 0) {   // a chunk the string reader skips
                s.put(0x7FFFu), s.put(8), s.put(libraryID(0x36003u)), s.put(0x11111111u), s.put(0x22222222u);
            }
            stringChunk(s, name, 0x36003u);
            stringChunk(s, rng.next() % 2u ? "" : "mask" + std::to_string(rng.next() % 3u), 0x36003u);
            s.put(rwID_EXTENSION), s.put(0), s.put(libraryID(0x36003u));
        };
        const auto runStream = [&](const Bytes& s, u32 entry, u32 extra, const auto& native) -> std::pair<u32, void*> {
            // a fresh texture ready for the guest's read callback; the native one makes its own inside
            instructions += g.call(kTextureCreate.entry, {0});
            guest_spare = g.r3();
            guest_loaded = false;
            g_native_loaded = nullptr;
            g_read_wants = rng.next() % 3u == 0;
            m.write_block(kData, s.b.data(), s.b.size());
            m.write32(kMem, kData), m.write32(kMem + 4, static_cast<u32>(s.b.size()));
            RwMemory memory{const_cast<u8*>(s.b.data()), s.b.size()};
            instructions += g.call(kStreamOpen.entry, {rwSTREAMMEMORY, rwSTREAMREAD, kMem});
            const u32 gs = g.r3();
            RwStream* ns = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &memory);
            if (extra == ~0u) instructions += g.call(entry, {gs});
            else instructions += g.call(entry, {extra, gs});
            const u32 gr = g.r3();
            void* nr = native(ns);
            instructions += g.call(kStreamClose.entry, {gs, 0});
            RwStreamClose(ns, nullptr);
            require(guest_loaded == (g_native_loaded != nullptr), "a texture loaded on one side only");
            if (guest_loaded) {   // the loaded texture joins the pool, named as the native callback named it
                const std::string n = RwTextureGetName(g_native_loaded);
                writeString(kName, n);
                g.call(kSetName.entry, {guest_spare, kName});
                gids[guest_spare] = next_id, g_native_ids[g_native_loaded] = next_id++;
                textures.push_back({guest_spare, g_native_loaded});
                // the pool's own reference (a material the read gave it to may destroy it otherwise)
                m.write32(guest_spare + 0x54, m.peek32(guest_spare + 0x54) + 1);
                ++g_native_loaded->refCount;
                ++loaded;
            } else {
                instructions += g.call(0x8048DDDCu, {guest_spare});   // RwTextureDestroy: the spare is not used
            }
            return {gr, nr};
        };

        for (unsigned step = 0; step < 15000; ++step) {
            const u32 op = rng.next() % 100u;
            const std::string what = "op " + std::to_string(op) + " step " + std::to_string(step);
            if (op < 10 && materials.size() < 30) {                                               // create
                instructions += g.call(kMatCreate.entry, {});
                const u32 gm = g.r3();
                RpMaterial* nm = RpMaterialCreate();
                require(gm != 0 && nm != nullptr, what + ": RpMaterialCreate");
                for (Seen& s : guest_seen) s.a = next_id;
                for (Seen& s : g_native_seen) s.a = next_id;
                gids[gm] = next_id, g_native_ids[nm] = next_id++;
                materials.push_back({gm, nm});
            } else if (op < 18 && !materials.empty()) {                                           // destroy
                const std::size_t k = rng.next() % materials.size();
                bool inList = false;
                for (const RpMaterialList& l : nativeLists) {
                    for (RwInt32 i = 0; i < l.numMaterials; ++i) inList = inList || l.materials[i] == materials[k].n;
                }
                const bool last = static_cast<RpMaterial*>(materials[k].n)->refCount == 1;
                instructions += g.call(kMatDestroy.entry, {materials[k].g});
                require(g.r3() == 1 && RpMaterialDestroy(static_cast<RpMaterial*>(materials[k].n)) == 1, what + ": RpMaterialDestroy");
                sameCallbacks(what);
                if (last) {
                    gids.erase(materials[k].g), g_native_ids.erase(materials[k].n);
                    materials.erase(materials.begin() + static_cast<long>(k));
                }
                (void)inList;
            } else if (op < 30 && !materials.empty()) {                                           // set texture
                const Pair& mat = materials[rng.next() % materials.size()];
                const bool none = rng.next() % 4u == 0;
                const Pair& t = textures[rng.next() % textures.size()];
                if (!none && static_cast<RwTexture*>(t.n)->refCount > 40) continue;
                instructions += g.call(kMatSetTexture.entry, {mat.g, none ? 0u : t.g});
                require(g.r3() == mat.g && RpMaterialSetTexture(static_cast<RpMaterial*>(mat.n), none ? nullptr : static_cast<RwTexture*>(t.n)) == mat.n,
                        what + ": RpMaterialSetTexture");
            } else if (op < 44 && !materials.empty()) {                                           // list append
                const u32 l = rng.next() % 2u;
                if (nativeLists[l].numMaterials > 70) continue;
                const Pair& mat = materials[rng.next() % materials.size()];
                instructions += g.call(kListAppend.entry, {kLists + 0x10u * l, mat.g});
                require(static_cast<RwInt32>(g.r3()) == _rpMaterialListAppendMaterial(&nativeLists[l], static_cast<RpMaterial*>(mat.n)),
                        what + ": _rpMaterialListAppendMaterial");
                ++appends;
            } else if (op < 52 && !materials.empty()) {                                           // find / get
                const u32 l = rng.next() % 2u;
                const Pair& mat = materials[rng.next() % materials.size()];
                instructions += g.call(kListFind.entry, {kLists + 0x10u * l, mat.g});
                const RwInt32 index = _rpMaterialListFindMaterialIndex(&nativeLists[l], static_cast<RpMaterial*>(mat.n));
                require(static_cast<RwInt32>(g.r3()) == index, what + ": _rpMaterialListFindMaterialIndex");
                if (index >= 0) {
                    instructions += g.call(kListGet.entry, {kLists + 0x10u * l, static_cast<u32>(index)});
                    require(gid(g.r3()) == nid(_rpMaterialListGetMaterial(&nativeLists[l], index)), what + ": _rpMaterialListGetMaterial");
                }
            } else if (op < 55) {                                                                 // list deinit
                const u32 l = rng.next() % 2u;
                std::vector<const void*> dying;
                for (RwInt32 i = 0; i < nativeLists[l].numMaterials; ++i) {
                    if (nativeLists[l].materials[i]->refCount == 1) dying.push_back(nativeLists[l].materials[i]);
                }
                instructions += g.call(kListDeinit.entry, {kLists + 0x10u * l});
                require(g.r3() == kLists + 0x10u * l && _rpMaterialListDeinitialize(&nativeLists[l]) == &nativeLists[l],
                        what + ": _rpMaterialListDeinitialize");
                sameCallbacks(what);
                for (const void* d : dying) {
                    for (std::size_t k = 0; k < materials.size(); ++k) {
                        if (materials[k].n == d) {
                            gids.erase(materials[k].g), g_native_ids.erase(materials[k].n);
                            materials.erase(materials.begin() + static_cast<long>(k));
                            break;
                        }
                    }
                }
            } else if (op < 80) {                                                                 // material stream
                Bytes s;
                const u32 length = rng.next() % 5u == 0 ? 4u * (rng.next() % 7u) : 28u;
                const u32 version = rng.next() % 15u == 0 ? 0x37003u : 0x36003u;
                const bool textured = rng.next() % 3u != 0;
                s.put(rwID_STRUCT), s.put(length), s.put(libraryID(version));
                const float surface[3] = {rng.real(-3, 3), rng.real(-3, 3), rng.real(-3, 3)};
                const u32 words[7] = {rng.next(), rng.next(), rng.next(), textured ? 1u + rng.next() % 3u : 0u,
                                      std::bit_cast<u32>(surface[0]), std::bit_cast<u32>(surface[1]), std::bit_cast<u32>(surface[2])};
                for (u32 i = 0; i < length / 4; ++i) s.put(words[i]);
                const bool hasTexture = length >= 16 && words[3] != 0;
                if (hasTexture) textureChunk(s, rng.next() % 4u != 0);
                s.put(rwID_EXTENSION), s.put(0), s.put(libraryID(0x36003u));
                const auto [gm, nmv] = runStream(s, kMatStreamRead.entry, ~0u, [](RwStream* st) -> void* { return RpMaterialStreamRead(st); });
                auto* nm = static_cast<RpMaterial*>(nmv);
                require((gm == 0) == (nm == nullptr), what + ": RpMaterialStreamRead succeeded on one side only");
                ++read;
                if (nm != nullptr) {
                    for (Seen& x : guest_seen) if (x.kind == 0) x.a = next_id;
                    for (Seen& x : g_native_seen) if (x.kind == 0) x.a = next_id;
                    gids[gm] = next_id, g_native_ids[nm] = next_id++;
                    materials.push_back({gm, nm});
                    readTextured += hasTexture;
                    readFoundTexture += nm->texture != nullptr;
                    if (materials.size() > 30) {
                        const Pair q = materials.front();
                        bool inList = false;
                        for (const RpMaterialList& l : nativeLists) {
                            for (RwInt32 i = 0; i < l.numMaterials; ++i) inList = inList || l.materials[i] == q.n;
                        }
                        if (!inList && static_cast<RpMaterial*>(q.n)->refCount == 1) {
                            sameCallbacks(what + " (read)");
                            instructions += g.call(kMatDestroy.entry, {q.g});
                            RpMaterialDestroy(static_cast<RpMaterial*>(q.n));
                            gids.erase(q.g), g_native_ids.erase(q.n);
                            materials.erase(materials.begin());
                        }
                    }
                }
            } else if (op < 92) {                                                                 // texture stream
                Bytes s;
                textureChunk(s, rng.next() % 3u != 0);
                // RwTextureStreamRead starts at the struct (its caller found the texture chunk): drop the 12-byte header
                s.b.erase(s.b.begin(), s.b.begin() + 12);
                const auto [gt, ntv] = runStream(s, kTexStreamRead.entry, ~0u, [](RwStream* st) -> void* { return RwTextureStreamRead(st); });
                require(gid(gt) == nid(ntv), what + ": RwTextureStreamRead");
            } else {                                                                              // string chunk
                Bytes s;
                if (rng.next() % 3u == 0) s.put(0x7FFEu), s.put(4), s.put(libraryID(0x36003u)), s.put(0x33333333u);
                std::string text;
                for (u32 i = rng.next() % 40u; i > 0; --i) text.push_back(static_cast<char>(' ' + rng.next() % 90u));
                stringChunk(s, text, rng.next() % 12u == 0 ? 0x34000u : 0x36003u);
                const bool own = rng.next() % 2u;
                RwChar buffer[64];
                std::memset(buffer, 0x5A, sizeof buffer);
                for (u32 i = 0; i < 64; ++i) m.write8(kStringBuf + i, 0x5A);
                const auto [gr, nrv] = runStream(s, kStringRead.entry, own ? 0u : kStringBuf, [&](RwStream* st) -> void* {
                    return _rwStringStreamFindAndRead(own ? nullptr : buffer, st);
                });
                require((gr == 0) == (nrv == nullptr), what + ": _rwStringStreamFindAndRead succeeded on one side only");
                if (nrv != nullptr) {
                    const auto* nt = static_cast<const RwChar*>(nrv);
                    const u32 length = (static_cast<u32>(text.size()) + 4u) & ~3u;
                    for (u32 i = 0; i < length; ++i) require(m.peek8(gr + i) == static_cast<u8>(nt[i]), what + ": string bytes");
                    if (own) RwFree(const_cast<RwChar*>(nt));   // the guest's stays in its heap
                    ++strings;
                }
            }
            sameCallbacks(what);
            sameAll(what);
            ++checks;
        }
        std::printf("  materials read %u (textured %u, textures found %u), textures loaded %u, list appends %u, strings %u\n",
                    read, readTextured, readFoundTexture, loaded, appends, strings);
        require(read > 2000 && readTextured > 800 && readFoundTexture > 300 && loaded > 300 && appends > 500 && strings > 300,
                "the sessions must reach every case");
        std::printf("rpworld_material_tests: %u operations identical to the original (%llu original instructions)\n", checks,
                    static_cast<unsigned long long>(instructions));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rpworld_material_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
