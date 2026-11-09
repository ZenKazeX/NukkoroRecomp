// Oracle for the native RenderWare lights (rpworld/src/light.cpp): the ORIGINAL balight functions of PAL main.dol in
// the reference interpreter (rwcore_oracle.h) and the native ones.
//   1. RpLightGetConeAngle (the inlined fdlibm acosf) over 400,000 values of -minusCosAngle: every branch (|x| tiny,
//      < 0.5, <= -0.5, >= 0.5, |x| >= 1, infinities, NaNs, denormals, both zeros), the result's bits compared.
//   2. Random sessions with a light plugin: create (every RW type and other bytes), destroy, colours (grey and not),
//      radius with and without a frame, cone angles (0 .. pi / 2, beyond, negative, NaN), frames attached / detached,
//      lights read from streams (short and long structs up to 24 bytes, bad versions, every type), RwGlobals'
//      lightFrame varied. After every operation: every light's bytes as fields (object bytes, frame identity, radius,
//      colour, cone, sector list empty and self-linked, world link, lightFrame), results, plugin callbacks, the held
//      error. At the end the module is closed with lights alive: the tidy destroys them, callbacks compared.
// Not generated (deliberate differences, rpworld_light_native_tests): failing plugin constructors, stream structs
// longer than 24 bytes, failing plugin reads, no light module.
// liveness-roots: reference-interpreter-only
//
// Usage: rpworld_light_tests <main.dol>
#include "rw/core/binary.h"
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/frame.h"
#include "rw/core/stream.h"
#include "rw/world/light.h"
#include "rw/world/world.h"
#include "rwcore_oracle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
using namespace rworacle;

constexpr Original kLightOpen{"_rpLightOpen", 0x8045BEE8u, 0x90u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kLightClose{"_rpLightClose", 0x8045BE70u, 0x78u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kCreate{"RpLightCreate", 0x8045BD84u, 0xECu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kDestroy{"RpLightDestroy", 0x8045BD24u, 0x60u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kStreamRead{"RpLightStreamRead", 0x8045BB6Cu, 0x1B8u, 0x9421FFC0u, 0x4E800020u};
constexpr Original kRegister{"RpLightRegisterPlugin", 0x8045BAF0u, 0x40u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRegisterStream{"RpLightRegisterPluginStream", 0x8045BB30u, 0x3Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSetCone{"RpLightSetConeAngle", 0x8045BA94u, 0x5Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kGetCone{"RpLightGetConeAngle", 0x8045B868u, 0x22Cu, 0x9421FFD0u, 0x4E800020u};
constexpr Original kSetColor{"RpLightSetColor", 0x8045B814u, 0x54u, 0x80A40000u, 0x4E800020u};
constexpr Original kSetRadius{"RpLightSetRadius", 0x8045B7D4u, 0x40u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kTidy{"LightTidyDestroyLight", 0x8045B7B0u, 0x20u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSync{"LightSync", 0x8045B7D0u, 0x4u, 0x4E800020u, 0x4E800020u};
constexpr Original kFrameOpen{"_rwFrameOpen", 0x804878D8u, 0xA4u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kFrameCreate{"RwFrameCreate", 0x80487D50u, 0x12Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSetFrame{"_rwObjectHasFrameSetFrame", 0x8048E810u, 0x74u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSyncDirty{"_rwFrameSyncDirty", 0x8048C3A4u, 0x6Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kMemoryOpen{"_rwMemoryOpen", 0x804812C4u, 0x118u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRegOpen{"_rwPluginRegistryOpen", 0x80482BE8u, 0x5Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kErrorOpen{"_rwErrorOpen", 0x8047EB88u, 0x38u, 0x390D6100u, 0x4E800020u};
constexpr Original kErrorGet{"RwErrorGet", 0x8047EC34u, 0x44u, 0x808D615Cu, 0x4E800020u};
constexpr Original kMatrixOpen{"_rwMatrixOpen", 0x8047F8DCu, 0xD4u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kVectorOpen{"_rwVectorOpen", 0x8048363Cu, 0x398u, 0x9421FFB0u, 0x4E800020u};
constexpr Original kStreamModuleOpen{"_rwStreamModuleOpen", 0x80481CBCu, 0x8Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kStreamOpen{"RwStreamOpen", 0x804823B8u, 0x288u, 0x9421FFC0u, 0x4E800020u};
constexpr Original kStreamClose{"RwStreamClose", 0x8048229Cu, 0x11Cu, 0x9421FFE0u, 0x4E800020u};
constexpr u32 kFreeListAlloc = 0x80480E00u, kFreeListFree = 0x80480FA0u;
constexpr u32 kStreamModule = 0x805F2628u, kLightModule = 0x805F2580u;   // streamModule, lightModule (r13 + 0x6080)
constexpr u32 kCtorStub = 0x817F0060u, kDtorStub = 0x817F0070u;
constexpr u32 kGuestErrorOffset = 0x200u, kGuestVectorOffset = 0x210u, kGuestMatrixOffset = 0x240u,
              kGuestFrameOffset = 0x280u, kGuestStreamOffset = 0x290u, kGuestLightOffset = 0x2B0u;
constexpr u32 kFuncs = kBuffer, kErr = kBuffer + 0x20u, kColour = kBuffer + 0x40u, kMem = kBuffer + 0xA0u,
              kData = 0x81010000u;
constexpr u32 kGuestLightFrame = kEngine + 0xAu;   // RwGlobals::lightFrame on the GameCube
constexpr unsigned kMaxLights = 24;

struct Seen {
    int kind;   // 0 ctor, 1 dtor
    int a;
    bool operator==(const Seen&) const = default;
};
std::vector<Seen> g_native_seen;
std::map<const void*, int> g_native_ids;
int nativeId(const void* p) {
    const auto it = g_native_ids.find(p);
    return it == g_native_ids.end() ? -1 : it->second;
}
void* nCtor(void* o, RwInt32, RwInt32) { g_native_seen.push_back({0, nativeId(o)}); return o; }
void* nDtor(void* o, RwInt32, RwInt32) { g_native_seen.push_back({1, nativeId(o)}); return o; }

struct Pair {
    u32 guest;
    RpLight* native;
};

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rpworld_light_tests main.dol");
        Guest g(argv[1]);
        for (const Original& f : {kLightOpen, kLightClose, kCreate, kDestroy, kStreamRead, kRegister, kRegisterStream,
                                  kSetCone, kGetCone, kSetColor, kSetRadius, kTidy, kSync, kFrameOpen, kFrameCreate,
                                  kSetFrame, kSyncDirty, kMemoryOpen, kRegOpen, kErrorOpen, kErrorGet, kMatrixOpen,
                                  kVectorOpen, kStreamModuleOpen, kStreamOpen, kStreamClose}) {
            g.check_pinned(f);
        }
        GuestMemory& m = g.memory();
        Rng rng;
        std::map<u32, int> guest_ids;
        const auto guestId = [&](u32 a) {
            const auto it = guest_ids.find(a);
            return it == guest_ids.end() ? -1 : it->second;
        };
        std::vector<Seen> guest_seen;
        g.stub(kCtorStub, [&](Cpu& c) { guest_seen.push_back({0, guestId(c.gpr[3])}); });
        g.stub(kDtorStub, [&](Cpu& c) { guest_seen.push_back({1, guestId(c.gpr[3])}); });

        // ---- the guest engine -----------------------------------------------------------------------------------
        g.reset_engine();
        for (u32 a : {kErrorModule, kErrorModule + 4u, kVectorModule, kVectorModule + 4u, kMatrixModule, kMatrixModule + 4u,
                      kStreamModule, kStreamModule + 4u, kLightModule, kLightModule + 4u}) {
            m.write32(a, 0);
        }
        m.write32(kFuncs, kMallocStub), m.write32(kFuncs + 4, kFreeStub), m.write32(kFuncs + 8, kReallocStub);
        m.write32(kFuncs + 12, 0);
        g.call(kMemoryOpen.entry, {kFuncs});
        m.write32(kEngine + 0x118u, kFreeListAlloc), m.write32(kEngine + 0x11Cu, kFreeListFree);
        g.call(kErrorOpen.entry, {kEngine, kGuestErrorOffset, 8});
        g.call(kRegOpen.entry, {});
        g.call(kVectorOpen.entry, {kEngine, kGuestVectorOffset, 8});
        g.call(kMatrixOpen.entry, {kEngine, kGuestMatrixOffset, 0x18});
        g.call(kStreamModuleOpen.entry, {kEngine, kGuestStreamOffset, 4});
        m.write32(0x805F2658u, 0);
        g.call(kRegister.entry, {8, 0x7012, kCtorStub, kDtorStub, 0});
        require(static_cast<int>(g.r3()) >= 0x40, "the guest light plugin");
        g.call(kFrameOpen.entry, {kEngine, kGuestFrameOffset, 4});
        g.call(kLightOpen.entry, {kEngine, kGuestLightOffset, 4});
        require(g.r3() == kEngine, "guest lights open");

        // ---- the native engine ----------------------------------------------------------------------------------
        RwEngineOpenParams params{nullptr};
        require(RwEngineInit(nullptr, 0, 0) == 1, "native init");
        require(RpWorldPluginAttach() == 1, "the world plugin");
        require(RpLightRegisterPlugin(8, 0x7012, nCtor, nDtor, nullptr) >= static_cast<RwInt32>(sizeof(RpLight)),
                "the native light plugin");
        require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "native start");
        RwGlobals& G = rwSrcGlobals();

        u64 instructions = 0;
        // ---- 1. the cone angle's acosf ----------------------------------------------------------------------------
        unsigned branches[5] = {};
        {
            instructions += g.call(kCreate.entry, {rpLIGHTSPOT});
            const u32 gl = g.r3();
            RpLight* nl = RpLightCreate(rpLIGHTSPOT);
            require(gl != 0 && nl != nullptr, "a spot light");
            for (unsigned i = 0; i < 400000; ++i) {
                u32 bits;
                const u32 kind = rng.next() % 10u;
                // the branch boundaries themselves (|x| = 2^-57 the tiny bound, 0.5, 1), both signs, first
                const u32 edges[] = {0x23000000u, 0x23000001u, 0x22FFFFFFu, 0x3F000000u, 0x3EFFFFFFu, 0x3F800000u, 0x3F7FFFFFu};
                if (i < 2 * std::size(edges)) bits = edges[i / 2] | (i % 2 ? 0x80000000u : 0u);
                else if (kind < 3) bits = std::bit_cast<u32>(-1.0f + 2.0f * static_cast<float>(rng.next() & 0xFFFFFF) / 16777216.0f);
                else if (kind < 5) bits = rng.next();                                         // any word
                else if (kind == 5) bits = (rng.next() & 0x80000000u) | (rng.next() % 0x23800000u);   // tiny
                else if (kind == 6) bits = (rng.next() & 0x80000000u) | (0x3EFFFFF0u + rng.next() % 0x20u);   // near 0.5
                else if (kind == 7) bits = (rng.next() & 0x80000000u) | (0x3F7FFFF0u + rng.next() % 0x20u);   // near 1
                else if (kind == 8) bits = std::bit_cast<u32>(rng.real(-30, 1));
                else bits = (rng.next() & 0x80000000u) | 0x7F800000u | (rng.next() % 2u ? rng.next() & 0x7FFFFFu : 0u);
                m.write32(gl + 0x28, bits);
                nl->minusCosAngle = std::bit_cast<float>(bits);
                instructions += g.call(kGetCone.entry, {gl});
                const float o = g.f1(), n = RpLightGetConeAngle(nl);
                if (std::bit_cast<u32>(o) != std::bit_cast<u32>(n)) {
                    char what[160];
                    std::snprintf(what, sizeof what, "RpLightGetConeAngle(%08X): original %08X, native %08X", bits,
                                  std::bit_cast<u32>(o), std::bit_cast<u32>(n));
                    require(false, what);
                }
                const u32 ix = (bits ^ 0x80000000u) & 0x7FFFFFFFu;   // x = -minusCosAngle
                const bool neg = !(bits & 0x80000000u) && ix != 0;
                ++branches[ix >= 0x3F800000u ? 0 : ix <= 0x23000000u ? 1 : ix < 0x3F000000u ? 2 : neg ? 3 : 4];
            }
            instructions += g.call(kDestroy.entry, {gl});
            RpLightDestroy(nl);
            guest_seen.clear(), g_native_seen.clear();
        }
        std::printf("  acosf: |x| >= 1 %u, tiny %u, < 0.5 %u, <= -0.5 %u, >= 0.5 %u\n", branches[0], branches[1], branches[2],
                    branches[3], branches[4]);
        require(*std::min_element(branches, branches + 5) > 10000, "every acosf branch");

        // ---- 2. sessions -----------------------------------------------------------------------------------------
        std::vector<Pair> lights;
        std::vector<std::pair<u32, RwFrame*>> frames;
        int next_id = 0;
        unsigned checks = 0, coneSet = 0, coneRefused = 0, streamsRead = 0, withFrame = 0;
        const auto sameCallbacks = [&](const std::string& what) {
            require(guest_seen == g_native_seen, what + ": callbacks");
            guest_seen.clear(), g_native_seen.clear();
            instructions += g.call(kErrorGet.entry, {kErr});
            RwError e{};
            RwErrorGet(&e);
            require(m.peek32(kErr) == static_cast<u32>(e.pluginID) && m.peek32(kErr + 4) == static_cast<u32>(e.errorCode),
                    what + ": the held error");
        };
        const auto word = [&](u32 ga, float n, const std::string& what) {
            char values[96];
            std::snprintf(values, sizeof values, " (original %08X, native %08X)", m.peek32(ga), std::bit_cast<u32>(n));
            require(m.peek32(ga) == std::bit_cast<u32>(n), what + values);
        };
        const auto sameAll = [&](const std::string& what) {
            for (const Pair& p : lights) {
                const RpLight& n = *p.native;
                const RwObject& o = n.object.object;
                require(m.peek8(p.guest) == o.type && m.peek8(p.guest + 1) == o.subType && m.peek8(p.guest + 2) == o.flags &&
                            m.peek8(p.guest + 3) == o.privateFlags,
                        what + ": object bytes");
                require(guestId(m.peek32(p.guest + 4)) == nativeId(o.parent), what + ": frame");
                word(p.guest + 0x14, n.radius, what + ": radius");
                word(p.guest + 0x18, n.color.red, what + ": red"), word(p.guest + 0x1C, n.color.green, what + ": green");
                word(p.guest + 0x20, n.color.blue, what + ": blue"), word(p.guest + 0x24, n.color.alpha, what + ": alpha");
                word(p.guest + 0x28, n.minusCosAngle, what + ": cone");
                require(m.peek32(p.guest + 0x2C) == p.guest + 0x2C && m.peek32(p.guest + 0x30) == p.guest + 0x2C &&
                            n.WorldSectorsInLight.link.next == &n.WorldSectorsInLight.link &&
                            n.WorldSectorsInLight.link.prev == &n.WorldSectorsInLight.link,
                        what + ": sector list empty");
                require(m.peek32(p.guest + 0x34) == 0 && m.peek32(p.guest + 0x38) == 0 && n.inWorld.next == nullptr &&
                            n.inWorld.prev == nullptr,
                        what + ": world link");
                require((m.peek32(p.guest + 0x3C) >> 16) == n.lightFrame, what + ": lightFrame");
                require(m.peek32(p.guest + 0x10) == kSync.entry && n.object.sync != nullptr, what + ": sync");
                if (const RwFrame* f = RpLightGetFrame(&n)) {   // the frame's pending update (RwFrameDirty: root flags & 3)
                    const u32 groot = m.peek32(m.peek32(p.guest + 4) + 0xA0);
                    require(((m.peek8(groot + 3) & 3u) != 0) == (RwFrameDirty(f) != 0), what + ": the frame's pending update");
                }
            }
        };
        const auto realIn = [&](float lo, float hi) { return lo + (hi - lo) * static_cast<float>(rng.next() & 0xFFFFFF) / 16777216.0f; };
        const auto addLight = [&](u32 gl, RpLight* nl) {
            const int id = next_id++;
            guest_ids[gl] = id, g_native_ids[nl] = id;
            lights.push_back({gl, nl});
        };
        const auto lightType = [&]() -> u32 {
            const u32 k = rng.next() % 8u;
            const u32 types[] = {rpLIGHTDIRECTIONAL, rpLIGHTAMBIENT, rpLIGHTPOINT, rpLIGHTSPOT, rpLIGHTSPOTSOFT};
            return k < 5 ? types[k] : rng.next() & 0xFFu;
        };

        for (unsigned step = 0; step < 20000; ++step) {
            const u32 op = rng.next() % 100u;
            if (lights.empty() || (op < 10 && lights.size() < kMaxLights)) {                     // create
                const u16 frame = static_cast<u16>(rng.next());
                m.write32(kEngine + 0x8u, (m.peek32(kEngine + 0x8u) & 0xFFFF0000u) | frame);
                G.lightFrame = frame;
                const u32 type = lightType();
                instructions += g.call(kCreate.entry, {type});
                const u32 gl = g.r3();
                RpLight* nl = RpLightCreate(static_cast<RwInt32>(type));
                require(gl != 0 && nl != nullptr, "RpLightCreate");
                guest_ids[gl] = next_id, g_native_ids[nl] = next_id;
                sameCallbacks("RpLightCreate");
                guest_ids.erase(gl), g_native_ids.erase(nl);
                addLight(gl, nl);
            } else if (op < 16 && lights.size() > 1) {                                            // destroy
                const std::size_t k = rng.next() % lights.size();
                const Pair p = lights[k];
                instructions += g.call(kDestroy.entry, {p.guest});
                require(g.r3() == 1 && RpLightDestroy(p.native) == 1, "RpLightDestroy");
                sameCallbacks("RpLightDestroy");
                guest_ids.erase(p.guest), g_native_ids.erase(p.native);
                lights.erase(lights.begin() + static_cast<long>(k));
            } else {
                Pair& p = lights[rng.next() % lights.size()];
                if (op < 34) {                                                                    // colour
                    RwRGBAReal c{realIn(0, 2), realIn(0, 2), realIn(0, 2), realIn(0, 1)};
                    if (rng.next() % 3u == 0) c.green = c.blue = c.red;                           // grey
                    if (rng.next() % 9u == 0) c.blue = -c.red;
                    g.write_float(kColour, c.red), g.write_float(kColour + 4, c.green);
                    g.write_float(kColour + 8, c.blue), g.write_float(kColour + 12, c.alpha);
                    instructions += g.call(kSetColor.entry, {p.guest, kColour});
                    require(g.r3() == p.guest && RpLightSetColor(p.native, &c) == p.native, "RpLightSetColor");
                } else if (op < 46) {                                                             // radius
                    const float r = rng.next() % 6u == 0 ? rng.real(-20, 20) : realIn(0, 500);
                    instructions += g.call(kSetRadius.entry, {p.guest}, {double(r)});
                    require(g.r3() == p.guest && RpLightSetRadius(p.native, r) == p.native, "RpLightSetRadius");
                    withFrame += RpLightGetFrame(p.native) != nullptr;
                } else if (op < 64) {                                                             // cone
                    const u32 kind = rng.next() % 8u;
                    const float a = kind < 4 ? realIn(0, 1.5707964f) : kind == 4 ? -realIn(0, 1) : kind == 5 ? realIn(1.5707964f, 4)
                                  : kind == 6 ? std::bit_cast<float>(0x3FC90FDBu) : std::bit_cast<float>(0x7FC00000u | (rng.next() & 0x3FFFFFu));
                    instructions += g.call(kSetCone.entry, {p.guest}, {double(a)});
                    RpLight* r = RpLightSetConeAngle(p.native, a);
                    require((g.r3() == p.guest) == (r == p.native) && (g.r3() == 0) == (r == nullptr), "RpLightSetConeAngle");
                    (r == nullptr ? coneRefused : coneSet) += 1;
                    instructions += g.call(kGetCone.entry, {p.guest});
                    require(std::bit_cast<u32>(g.f1()) == std::bit_cast<u32>(RpLightGetConeAngle(p.native)),
                            "RpLightGetConeAngle after a set");
                } else if (op < 76) {                                                             // frames
                    if (frames.size() < 12 || rng.next() % 3u == 0) {
                        instructions += g.call(kFrameCreate.entry, {});
                        frames.push_back({g.r3(), RwFrameCreate()});
                        guest_ids[frames.back().first] = 1000 + static_cast<int>(frames.size());
                        g_native_ids[frames.back().second] = 1000 + static_cast<int>(frames.size());
                    }
                    const bool detach = rng.next() % 4u == 0;
                    const auto& f = frames[rng.next() % frames.size()];
                    instructions += g.call(kSetFrame.entry, {p.guest, detach ? 0u : f.first});
                    RpLightSetFrame(p.native, detach ? nullptr : f.second);
                    if (rng.next() & 1u) {
                        instructions += g.call(kSyncDirty.entry, {});
                        _rwFrameSyncDirty();
                    }
                } else {                                                                          // stream
                    std::vector<u8> data;
                    const auto put = [&](u32 w) { for (int i = 0; i < 4; ++i) data.push_back(static_cast<u8>(w >> (8 * i))); };
                    const u32 length = rng.next() % 4u == 0 ? 4u * (rng.next() % 6u) : 24u;
                    const u32 version = rng.next() % 10u == 0 ? (rng.next() & 1u ? 0x34000u : 0x37003u) : 0x36003u;
                    const u32 id = (((version - 0x30000u) << 14) & 0xFFC00000u) | ((version & 0x3Fu) << 16) | 0x37u;
                    put(rwID_STRUCT), put(length), put(id);
                    const u32 fields[6] = {std::bit_cast<u32>(realIn(0, 300)), std::bit_cast<u32>(realIn(0, 1)),
                                           std::bit_cast<u32>(realIn(0, 1)), std::bit_cast<u32>(rng.next() % 2u ? realIn(0, 1) : 0.5f),
                                           std::bit_cast<u32>(-realIn(0, 1)), (lightType() << 16) | (rng.next() & 0xFFu)};
                    for (u32 i = 0; i < length / 4; ++i) put(fields[i]);
                    put(rwID_EXTENSION), put(0), put(id);
                    m.write_block(kData, data.data(), data.size());
                    m.write32(kMem, kData), m.write32(kMem + 4, static_cast<u32>(data.size()));
                    RwMemory memory{data.data(), data.size()};
                    instructions += g.call(kStreamOpen.entry, {rwSTREAMMEMORY, rwSTREAMREAD, kMem});
                    const u32 gs = g.r3();
                    RwStream* ns = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &memory);
                    instructions += g.call(kStreamRead.entry, {gs});
                    const u32 gl = g.r3();
                    RpLight* nl = RpLightStreamRead(ns);
                    require((gl == 0) == (nl == nullptr), "RpLightStreamRead succeeded on one side only");
                    instructions += g.call(kStreamClose.entry, {gs, 0});
                    RwStreamClose(ns, nullptr);
                    if (nl != nullptr) {
                        guest_ids[gl] = next_id, g_native_ids[nl] = next_id;
                        sameCallbacks("RpLightStreamRead");
                        guest_ids.erase(gl), g_native_ids.erase(nl);
                        addLight(gl, nl);
                        ++streamsRead;
                        if (lights.size() > kMaxLights) {
                            const Pair q = lights.front();
                            instructions += g.call(kDestroy.entry, {q.guest});
                            RpLightDestroy(q.native);
                            sameCallbacks("RpLightDestroy");
                            guest_ids.erase(q.guest), g_native_ids.erase(q.native);
                            lights.erase(lights.begin());
                        }
                    }
                }
            }
            sameCallbacks("after operation " + std::to_string(op) + " of step " + std::to_string(step));
            sameAll("after operation " + std::to_string(op) + " of step " + std::to_string(step));
            ++checks;
        }
        // ---- the module closes with lights alive: the tidy destroys every one --------------------------------------
        const std::size_t alive = lights.size();
        instructions += g.call(kLightClose.entry, {kEngine, kGuestLightOffset, 4});
        _rpLightClose(RwEngineInstance, lightModule.globalsOffset, 4);
        std::sort(guest_seen.begin(), guest_seen.end(), [](const Seen& a, const Seen& b) { return a.a < b.a; });
        std::sort(g_native_seen.begin(), g_native_seen.end(), [](const Seen& a, const Seen& b) { return a.a < b.a; });
        require(guest_seen.size() == alive && guest_seen == g_native_seen, "the close destroyed every light");
        guest_seen.clear(), g_native_seen.clear();
        std::printf("  cone angles set %u, refused %u; radius changes on framed lights %u; lights read %u; %zu alive at the close\n",
                    coneSet, coneRefused, withFrame, streamsRead, alive);
        require(coneSet > 1000 && coneRefused > 300 && withFrame > 200 && streamsRead > 500 && alive > 5,
                "the sessions must reach every case");
        std::printf("rpworld_light_tests: 400000 cone angles and %u operations identical to the original (%llu original "
                    "instructions)\n",
                    checks, static_cast<unsigned long long>(instructions));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rpworld_light_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
