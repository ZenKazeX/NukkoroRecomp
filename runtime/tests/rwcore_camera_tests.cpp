// Oracle for the native RenderWare cameras (rwcore/src/camera.cpp): the ORIGINAL bacamera / babincam / babbox
// functions of PAL main.dol in the reference interpreter (rwcore_oracle.h) and the native ones run the same random
// sessions: cameras on frame chains whose frames are rotated / translated / scaled, view windows of every common
// aspect (4:3, 16:9, 16:10, 21:9, 32:10) and random / extreme ones, offsets, near / far planes (tiny, huge, equal,
// zero, negative), both projections and invalid ones, device Z ranges, dirty syncs, begin / end update, clear, show
// raster (device standard functions scripted on both sides), frustum sphere tests, cameras read from streams, a
// camera plugin. After every operation: every camera's fields (projection, view window and its reciprocal, offset,
// planes, fog, Z scale / shift) bit for bit; wherever its frame has no pending update, its view matrix, frustum
// corners, six planes with their closest-corner bytes and bounding box bit for bit; results, callbacks in order, the
// current camera, the held error. One hardware difference is allowed: a NaN the arithmetic GENERATES (0 / 0, inf * 0 -
// degenerate windows or clip ranges) is 0x7FC00000 on the Gekko and 0xFFC00000 on x86, so any two NaNs count as equal,
// and a plane's closest-corner byte (the sign of its normal) is not compared where that normal is NaN: every dot
// product with it is NaN whichever corner is taken, so no result depends on it. Every comparison of a NaN is still
// checked through the results (sphere tests).
//
// Not generated (deliberate differences, rwcore_camera_native_tests): a failed begin-update's current camera (the
// native restores it), destroying the current camera, stream chunks longer than 32 bytes, failing plugin constructors.
// liveness-roots: reference-interpreter-only
//
// Usage: rwcore_camera_tests <main.dol>
#include "rw/core/binary.h"
#include "rw/core/camera.h"
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/frame.h"
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

constexpr Original kCamOpen{"_rwCameraOpen", 0x80486474u, 0x90u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kEndUpdate{"RwCameraEndUpdate", 0x80486504u, 0x28u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kBeginUpdate{"RwCameraBeginUpdate", 0x8048652Cu, 0x28u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSetOffset{"RwCameraSetViewOffset", 0x80486554u, 0x4Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSetNear{"RwCameraSetNearClipPlane", 0x804865A0u, 0xBCu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSetFar{"RwCameraSetFarClipPlane", 0x8048665Cu, 0xBCu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kTestSphere{"RwCameraFrustumTestSphere", 0x80486718u, 0xF4u, 0xC104000Cu, 0x4E800020u};
constexpr Original kClear{"RwCameraClear", 0x8048680Cu, 0x4Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kShowRaster{"RwCameraShowRaster", 0x80486858u, 0x44u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSetProjection{"RwCameraSetProjection", 0x8048689Cu, 0x104u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kSetWindow{"RwCameraSetViewWindow", 0x804869A0u, 0x68u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kCamRegister{"RwCameraRegisterPlugin", 0x80486A08u, 0x40u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kCamDestroy{"RwCameraDestroy", 0x80486A48u, 0x60u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kCamCreate{"RwCameraCreate", 0x80486AA8u, 0x174u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kStreamRead{"RwCameraStreamRead", 0x804845E0u, 0x15Cu, 0x9421FFC0u, 0x4E800020u};
constexpr Original kFrameOpen{"_rwFrameOpen", 0x804878D8u, 0xA4u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kFrameCreate{"RwFrameCreate", 0x80487D50u, 0x12Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kFrameAdd{"RwFrameAddChild", 0x80488398u, 0x100u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kTranslate{"RwFrameTranslate", 0x80488600u, 0x9Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kScale{"RwFrameScale", 0x8048869Cu, 0x9Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRotate{"RwFrameRotate", 0x804887D4u, 0x9Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSyncDirty{"_rwFrameSyncDirty", 0x8048C3A4u, 0x6Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSetFrame{"_rwObjectHasFrameSetFrame", 0x8048E810u, 0x74u, 0x9421FFF0u, 0x4E800020u};
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
constexpr u32 kStreamModule = 0x805F2628u;   // streamModule
constexpr u32 kCtorStub = 0x817F0060u, kDtorStub = 0x817F0070u, kBeginStub = 0x817F0080u, kEndStub = 0x817F0090u,
              kShowStub = 0x817F00A0u, kClearStub = 0x817F00B0u;
constexpr u32 kGuestErrorOffset = 0x200u, kGuestVectorOffset = 0x210u, kGuestMatrixOffset = 0x240u,
              kGuestFrameOffset = 0x280u, kGuestStreamOffset = 0x290u, kGuestCameraOffset = 0x2A0u;
constexpr u32 kFuncs = kBuffer, kErr = kBuffer + 0x20u, kVec = kBuffer + 0x40u, kSphere = kBuffer + 0x60u,
              kColour = kBuffer + 0x80u, kMem = kBuffer + 0xA0u, kData = 0x81010000u;
constexpr unsigned kMaxCameras = 8;

// ---- callbacks seen on each side ---------------------------------------------------------------------------------
struct Seen {
    int kind;   // 0 ctor, 1 dtor, 2 begin, 3 end, 4 show, 5 clear
    int a;      // camera identity (-1 new / unknown)
    u32 b;      // clear mode / show flags
    bool operator==(const Seen&) const = default;
};
std::vector<Seen> g_native_seen;
std::vector<u32> g_script;
std::size_t g_native_next = 0;
u32 script(std::size_t& next) { return g_script[next++ % g_script.size()]; }
std::map<const void*, int> g_native_ids;
int nativeId(const void* p) {
    const auto it = g_native_ids.find(p);
    return it == g_native_ids.end() ? -1 : it->second;
}
void* nCtor(void* o, RwInt32, RwInt32) { g_native_seen.push_back({0, nativeId(o), 0}); return o; }
void* nDtor(void* o, RwInt32, RwInt32) { g_native_seen.push_back({1, nativeId(o), 0}); return o; }
RwBool nBegin(void*, void* camera, RwInt32) { g_native_seen.push_back({2, nativeId(camera), 0}); return script(g_native_next) % 6u != 0; }
RwBool nEnd(void*, void* camera, RwInt32) { g_native_seen.push_back({3, nativeId(camera), 0}); return script(g_native_next) % 6u != 0; }
RwBool nShow(void* raster, void*, RwInt32 flags) {
    g_native_seen.push_back({4, raster == nullptr ? -10 : 0, static_cast<u32>(flags)});
    return script(g_native_next) % 3u != 0;
}
RwBool nClear(void* camera, void*, RwInt32 mode) {
    g_native_seen.push_back({5, nativeId(camera), static_cast<u32>(mode)});
    return script(g_native_next) % 3u != 0;
}

bool isNaN(u32 bits) { return (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x7FFFFFu) != 0; }
unsigned g_nan_words = 0;   // words that differ but are both NaN (reported: the rule must stay rare)

struct Cam {
    u32 guest;
    RwCamera* native;
    u32 gframe, gparent;           // the camera's frame and its parent (a chain of two)
    RwFrame *nframe, *nparent;
};

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rwcore_camera_tests main.dol");
        Guest g(argv[1]);
        for (const Original& f : {kCamOpen, kEndUpdate, kBeginUpdate, kSetOffset, kSetNear, kSetFar, kTestSphere, kClear,
                                  kShowRaster, kSetProjection, kSetWindow, kCamRegister, kCamDestroy, kCamCreate,
                                  kStreamRead, kFrameOpen, kFrameCreate, kFrameAdd, kTranslate, kScale, kRotate,
                                  kSyncDirty, kSetFrame, kMemoryOpen, kRegOpen, kErrorOpen, kErrorGet, kMatrixOpen,
                                  kVectorOpen, kStreamModuleOpen, kStreamOpen, kStreamClose}) {
            g.check_pinned(f);
        }
        GuestMemory& m = g.memory();
        Rng rng;
        for (unsigned i = 0; i < 97; ++i) g_script.push_back(rng.next());

        std::map<u32, int> guest_ids;
        const auto guestId = [&](u32 a) {
            const auto it = guest_ids.find(a);
            return it == guest_ids.end() ? -1 : it->second;
        };
        std::vector<Seen> guest_seen;
        std::size_t guest_next = 0;
        g.stub(kCtorStub, [&](Cpu& c) { guest_seen.push_back({0, guestId(c.gpr[3]), 0}); });
        g.stub(kDtorStub, [&](Cpu& c) { guest_seen.push_back({1, guestId(c.gpr[3]), 0}); });
        g.stub(kBeginStub, [&](Cpu& c) {
            guest_seen.push_back({2, guestId(c.gpr[4]), 0});
            c.gpr[3] = script(guest_next) % 6u != 0;
        });
        g.stub(kEndStub, [&](Cpu& c) {
            guest_seen.push_back({3, guestId(c.gpr[4]), 0});
            c.gpr[3] = script(guest_next) % 6u != 0;
        });
        g.stub(kShowStub, [&](Cpu& c) {
            guest_seen.push_back({4, c.gpr[3] == 0 ? -10 : 0, c.gpr[5]});
            c.gpr[3] = script(guest_next) % 3u != 0;
        });
        g.stub(kClearStub, [&](Cpu& c) {
            guest_seen.push_back({5, guestId(c.gpr[3]), c.gpr[5]});
            c.gpr[3] = script(guest_next) % 3u != 0;
        });
        // the arena purge RwRasterShowRaster runs first is rwcore_resources_tests' (the native one purges an empty
        // arena here): a no-op on the guest side, whose resources module is not set up
        g.stub(0x8048181Cu, [](Cpu&) {});

        // ---- the guest engine -----------------------------------------------------------------------------------
        g.reset_engine();
        for (u32 a : {kErrorModule, kErrorModule + 4u, kVectorModule, kVectorModule + 4u, kMatrixModule, kMatrixModule + 4u,
                      kStreamModule, kStreamModule + 4u}) {
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
        g.call(kCamRegister.entry, {8, 0x7005, kCtorStub, kDtorStub, 0});
        require(static_cast<int>(g.r3()) >= 0x184, "the guest camera plugin");
        g.call(kFrameOpen.entry, {kEngine, kGuestFrameOffset, 4});
        g.call(kCamOpen.entry, {kEngine, kGuestCameraOffset, 4});
        require(g.r3() == kEngine, "guest cameras open");
        // the device's standard functions (RwGlobals::stdFunc at +0x48 on the GameCube)
        m.write32(kEngine + 0x48u + 4u * 1u, kBeginStub);
        m.write32(kEngine + 0x48u + 4u * 10u, kEndStub);
        m.write32(kEngine + 0x48u + 4u * 20u, kShowStub);
        m.write32(kEngine + 0x48u + 4u * 21u, kClearStub);

        // ---- the native engine ----------------------------------------------------------------------------------
        RwEngineOpenParams params{nullptr};
        require(RwEngineInit(nullptr, 0, 0) == 1, "native init");
        require(RwCameraRegisterPlugin(8, 0x7005, nCtor, nDtor, nullptr) >= static_cast<RwInt32>(sizeof(RwCamera)),
                "the native camera plugin");
        require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "native start");
        RwGlobals& G = rwSrcGlobals();
        G.stdFunc[1] = nBegin, G.stdFunc[10] = nEnd, G.stdFunc[20] = nShow, G.stdFunc[21] = nClear;

        u64 instructions = 0;
        unsigned checks = 0, framesCompared = 0, parallelCompared = 0, spheres = 0, streamsRead = 0;
        unsigned sphereResults[3] = {}, refused = 0, projectionErrors = 0;
        int next_id = 0;
        std::vector<Cam> cams;
        const auto setZRange = [&](float zn, float zf) {
            g.write_float(kEngine + 0x18u, zn), g.write_float(kEngine + 0x1Cu, zf);
            G.dOpenDevice.zBufferNear = zn, G.dOpenDevice.zBufferFar = zf;
        };
        setZRange(0.0f, 1.0f);
        const auto sameWords = [&](u32 ga, const void* n, u32 words, const std::string& what) {
            const auto* w = static_cast<const u32*>(n);
            for (u32 i = 0; i < words; ++i) {
                if (isNaN(m.peek32(ga + 4 * i)) && isNaN(w[i])) {
                    g_nan_words += m.peek32(ga + 4 * i) != w[i];
                    continue;
                }
                char values[64];
                std::snprintf(values, sizeof values, " (word %u: original %08X, native %08X)", i, m.peek32(ga + 4 * i), w[i]);
                require(m.peek32(ga + 4 * i) == w[i], what + values);
            }
        };
        const auto sameCallbacks = [&](const char* what) {
            if (guest_seen != g_native_seen) {
                std::string seen = std::string(what) + ": callbacks differ; original";
                for (const Seen& x : guest_seen) seen += " " + std::to_string(x.kind) + "/" + std::to_string(x.a);
                seen += ", native";
                for (const Seen& x : g_native_seen) seen += " " + std::to_string(x.kind) + "/" + std::to_string(x.a);
                require(false, seen);
            }
            guest_seen.clear(), g_native_seen.clear();
            instructions += g.call(kErrorGet.entry, {kErr});
            RwError e{};
            RwErrorGet(&e);
            char errors[96];
            std::snprintf(errors, sizeof errors, ": errors differ (original %08X/%08X, native %08X/%08X)", m.peek32(kErr),
                          m.peek32(kErr + 4), static_cast<u32>(e.pluginID), static_cast<u32>(e.errorCode));
            require(m.peek32(kErr) == static_cast<u32>(e.pluginID) && m.peek32(kErr + 4) == static_cast<u32>(e.errorCode),
                    what + std::string(errors));
        };
        const auto sameAll = [&](const std::string& what) {
            for (const Cam& c : cams) {
                const RwCamera& n = *c.native;
                require(m.peek32(c.guest + 0x14) == static_cast<u32>(n.projectionType), what + ": projection");
                sameWords(c.guest + 0x68, &n.viewWindow, 2, what + ": view window");
                sameWords(c.guest + 0x70, &n.recipViewWindow, 2, what + ": recip view window");
                sameWords(c.guest + 0x78, &n.viewOffset, 2, what + ": view offset");
                sameWords(c.guest + 0x80, &n.nearPlane, 5, what + ": near / far / fog / zScale / zShift");
                require(guestId(m.peek32(c.guest + 4)) == nativeId(n.object.object.parent), what + ": the camera's frame");
                const RwFrame* f = static_cast<const RwFrame*>(n.object.object.parent);
                // whether the frame's hierarchy has an update pending (RwFrameDirty 0x80487D40: the root's private
                // flags & 3) - both sides synced at the same moments, before begin-update's device call included
                if (f != nullptr) {
                    const u32 groot = m.peek32(c.guest + 4 + 0) == 0 ? 0 : m.peek32(m.peek32(c.guest + 4) + 0xA0);
                    require(((m.peek8(groot + 3) & 3u) != 0) == (RwFrameDirty(f) != 0), what + ": the frame's pending update");
                }
                if (f != nullptr && !RwFrameDirty(f)) {
                    // the view matrix (rows and flags; the pads are the original's memory)
                    const auto* vm = reinterpret_cast<const u32*>(&n.viewMatrix);
                    for (u32 i = 0; i < 16; ++i) {
                        if (i == 7 || i == 11 || i == 15) continue;
                        const u32 o = m.peek32(c.guest + 0x20 + 4 * i);
                        require(o == vm[i] || (i != 3 && isNaN(o) && isNaN(vm[i])), what + ": view matrix word " + std::to_string(i));
                    }
                    for (int p = 0; p < 6; ++p) {
                        const u32 gp = c.guest + 0x94 + 20u * static_cast<u32>(p);
                        sameWords(gp, &n.frustumPlanes[p].plane, 4, what + ": plane " + std::to_string(p));
                        const RwUInt8 closest[3] = {n.frustumPlanes[p].closestX, n.frustumPlanes[p].closestY,
                                                    n.frustumPlanes[p].closestZ};
                        for (u32 k = 0; k < 3; ++k) {
                            require(isNaN(m.peek32(gp + 4 * k)) || m.peek8(gp + 16 + k) == closest[k],
                                    what + ": plane " + std::to_string(p) + " closest corner");
                        }
                    }
                    sameWords(c.guest + 0x10C, &n.frustumBoundBox, 6, what + ": frustum box");
                    sameWords(c.guest + 0x124, n.frustumCorners, 24, what + ": frustum corners");
                    ++framesCompared;
                    parallelCompared += n.projectionType == rwPARALLEL;
                }
            }
            require(guestId(m.peek32(kEngine)) == nativeId(G.curCamera), what + ": the current camera");
        };
        const auto realIn = [&](float lo, float hi) {   // a random float in [lo, hi]
            return lo + (hi - lo) * static_cast<float>(rng.next() & 0xFFFFFF) / 16777216.0f;
        };
        const auto window = [&]() -> RwV2d {
            const u32 kind = rng.next() % 16u;
            const float aspects[] = {4.0f / 3.0f, 16.0f / 9.0f, 16.0f / 10.0f, 21.0f / 9.0f, 32.0f / 10.0f};
            if (kind < 10) {
                const float h = realIn(0.1f, 2.0f);
                return {h * aspects[kind % 5u], h};   // Hor+: the width follows the aspect
            }
            if (kind == 10) return {rng.real(-20, 20), rng.real(-20, 20)};   // any size, any sign (zero now and then)
            if (kind == 11) return {realIn(1e-6f, 1e-5f), realIn(1e4f, 1e5f)};   // extreme shapes
            if (kind == 12) return {realIn(1e4f, 1e5f), realIn(1e-6f, 1e-5f)};
            if (kind == 13) return {-realIn(0.1f, 2.0f), realIn(0.1f, 2.0f)};   // a mirrored window
            return {1.0f, 1.0f};
        };
        const auto newFramePair = [&](Cam& c) {
            instructions += g.call(kFrameCreate.entry, {});
            c.gparent = g.r3();
            instructions += g.call(kFrameCreate.entry, {});
            c.gframe = g.r3();
            c.nparent = RwFrameCreate(), c.nframe = RwFrameCreate();
            guest_ids[c.gparent] = 500 + next_id, g_native_ids[c.nparent] = 500 + next_id;
            guest_ids[c.gframe] = 600 + next_id, g_native_ids[c.nframe] = 600 + next_id;
            instructions += g.call(kFrameAdd.entry, {c.gparent, c.gframe});
            RwFrameAddChild(c.nparent, c.nframe);
        };

        for (unsigned step = 0; step < 30000; ++step) {
            const u32 op = rng.next() % 100u;
            if (cams.empty() || (op < 4 && cams.size() < kMaxCameras)) {                       // create + frame
                instructions += g.call(kCamCreate.entry, {});
                const u32 gc = g.r3();
                RwCamera* nc = RwCameraCreate();
                require(gc != 0 && nc != nullptr, "RwCameraCreate");
                Cam c{gc, nc, 0, 0, nullptr, nullptr};
                const int id = next_id++;
                guest_ids[gc] = id, g_native_ids[nc] = id;
                sameCallbacks("RwCameraCreate's callbacks");
                newFramePair(c);
                instructions += g.call(kSetFrame.entry, {gc, c.gframe});
                RwCameraSetFrame(nc, c.nframe);
                cams.push_back(c);
            } else if (op < 6 && cams.size() > 1) {                                             // destroy
                const std::size_t k = rng.next() % cams.size();
                const Cam c = cams[k];
                if (G.curCamera == c.native) continue;   // the current camera: a native difference
                instructions += g.call(kCamDestroy.entry, {c.guest});
                require(g.r3() == 1 && RwCameraDestroy(c.native) == 1, "RwCameraDestroy");
                sameCallbacks("RwCameraDestroy's callbacks");
                guest_ids.erase(c.guest), g_native_ids.erase(c.native);
                cams.erase(cams.begin() + static_cast<long>(k));
            } else {
                Cam& c = cams[rng.next() % cams.size()];
                if (op < 20) {                                                                  // view window
                    const RwV2d w = window();
                    g.write_float(kVec, w.x), g.write_float(kVec + 4, w.y);
                    instructions += g.call(kSetWindow.entry, {c.guest, kVec});
                    require(g.r3() == c.guest && RwCameraSetViewWindow(c.native, &w) == c.native, "RwCameraSetViewWindow");
                } else if (op < 26) {                                                           // offset
                    const RwV2d o = rng.next() % 3u == 0 ? RwV2d{0, 0} : RwV2d{rng.real(-6, 2), rng.real(-6, 2)};
                    g.write_float(kVec, o.x), g.write_float(kVec + 4, o.y);
                    instructions += g.call(kSetOffset.entry, {c.guest, kVec});
                    require(g.r3() == c.guest && RwCameraSetViewOffset(c.native, &o) == c.native, "RwCameraSetViewOffset");
                } else if (op < 36) {                                                           // near / far
                    const u32 kind = rng.next() % 24u;
                    const float v = kind == 0 ? 0.0f : kind == 1 ? -realIn(0.1f, 5.0f) : kind == 2 ? realIn(1e-7f, 1e-5f)
                                  : kind == 3 ? realIn(1e6f, 1e9f) : kind == 4 ? c.native->farPlane
                                  : realIn(0.01f, 2000.0f);
                    const bool isFar = rng.next() & 1u;
                    instructions += g.call(isFar ? kSetFar.entry : kSetNear.entry, {c.guest}, {double(v)});
                    RwCamera* r = isFar ? RwCameraSetFarClipPlane(c.native, v) : RwCameraSetNearClipPlane(c.native, v);
                    require(g.r3() == c.guest && r == c.native, "RwCameraSetNear / FarClipPlane");
                } else if (op < 40) {                                                           // projection
                    const u32 pick = rng.next() % 6u;
                    const u32 proj = pick < 2 ? 1u : pick < 4 ? 2u : pick == 4 ? 0u : 3u + rng.next() % 5u;
                    instructions += g.call(kSetProjection.entry, {c.guest, proj});
                    RwCamera* r = RwCameraSetProjection(c.native, static_cast<RwCameraProjection>(proj));
                    projectionErrors += r == nullptr;
                    require((g.r3() == c.guest) == (r == c.native) && (g.r3() == 0) == (r == nullptr), "RwCameraSetProjection");
                    sameCallbacks("RwCameraSetProjection's error");
                } else if (op < 42) {                                                           // the device's Z range
                    const float zn = rng.next() % 2u ? 0.0f : realIn(-1.0f, 1.0f), zf = rng.next() % 2u ? 1.0f : realIn(0.5f, 70000.0f);
                    setZRange(zn, zf);
                } else if (op < 58) {                                                           // move the frames
                    const bool parent = rng.next() & 1u;
                    const u32 gf = parent ? c.gparent : c.gframe;
                    RwFrame* nf = parent ? c.nparent : c.nframe;
                    const RwV3d v{rng.real(-8, 8), rng.real(-8, 8), rng.real(-8, 8)};
                    g.write_float(kVec, v.x), g.write_float(kVec + 4, v.y), g.write_float(kVec + 8, v.z);
                    const RwOpCombineType combine = static_cast<RwOpCombineType>(rng.next() % 3u);
                    const u32 kind = rng.next() % 4u;
                    if (kind == 0) {
                        instructions += g.call(kTranslate.entry, {gf, kVec, static_cast<u32>(combine)});
                        RwFrameTranslate(nf, &v, combine);
                    } else if (kind == 1) {
                        const RwV3d s{realIn(0.2f, 3.0f), realIn(0.2f, 3.0f), realIn(0.2f, 3.0f)};
                        g.write_float(kVec, s.x), g.write_float(kVec + 4, s.y), g.write_float(kVec + 8, s.z);
                        instructions += g.call(kScale.entry, {gf, kVec, static_cast<u32>(combine)});
                        RwFrameScale(nf, &s, combine);
                    } else {
                        const float angle = realIn(-400.0f, 400.0f);
                        instructions += g.call(kRotate.entry, {gf, kVec, static_cast<u32>(combine)}, {double(angle)});
                        RwFrameRotate(nf, &v, angle, combine);
                    }
                } else if (op < 66) {                                                           // the dirty sync
                    instructions += g.call(kSyncDirty.entry, {});
                    _rwFrameSyncDirty();
                } else if (op < 72) {                                                           // begin / end update
                    if (G.curCamera == nullptr) {
                        instructions += g.call(kBeginUpdate.entry, {c.guest});
                        RwCamera* r = RwCameraBeginUpdate(c.native);
                        require((g.r3() == c.guest) == (r == c.native) && (g.r3() == 0) == (r == nullptr), "RwCameraBeginUpdate");
                        if (r == nullptr) m.write32(kEngine, 0), ++refused;   // the native restores curCamera
                        sameCallbacks("RwCameraBeginUpdate's callbacks");
                    } else {
                        const Cam& cur = *std::find_if(cams.begin(), cams.end(), [&](const Cam& x) { return x.native == G.curCamera; });
                        instructions += g.call(kEndUpdate.entry, {cur.guest});
                        RwCamera* r = RwCameraEndUpdate(cur.native);
                        require((g.r3() == cur.guest) == (r == cur.native) && (g.r3() == 0) == (r == nullptr), "RwCameraEndUpdate");
                        sameCallbacks("RwCameraEndUpdate's callbacks");
                    }
                } else if (op < 76) {                                                           // clear / show
                    if (rng.next() & 1u) {
                        const u32 mode = rng.next() % 8u;
                        instructions += g.call(kClear.entry, {c.guest, kColour, mode});
                        RwCamera* r = RwCameraClear(c.native, nullptr, static_cast<RwInt32>(mode));
                        require((g.r3() == c.guest) == (r == c.native) && (g.r3() == 0) == (r == nullptr), "RwCameraClear");
                    } else {
                        const u32 flags = rng.next() % 4u;
                        const bool raster = rng.next() & 1u;   // a frame buffer (the device only sees its pointer)
                        static unsigned char nativeRaster[16];
                        m.write32(c.guest + 0x60, raster ? kData - 0x100u : 0u);
                        c.native->frameBuffer = raster ? reinterpret_cast<RwRaster*>(nativeRaster) : nullptr;
                        instructions += g.call(kShowRaster.entry, {c.guest, 0, flags});
                        RwCamera* r = RwCameraShowRaster(c.native, nullptr, flags);
                        require((g.r3() == c.guest) == (r == c.native) && (g.r3() == 0) == (r == nullptr), "RwCameraShowRaster");
                    }
                    sameCallbacks("RwCameraClear / ShowRaster's callbacks");
                } else if (op < 94) {                                                           // sphere tests
                    if (!RwFrameDirty(c.nframe)) {
                        for (int i = 0; i < 8; ++i) {
                            const u32 kind = rng.next() % 3u;
                            RwSphere s{};
                            if (kind == 2) {   // near the frustum's centroid, small: mostly inside
                                RwV3d centroid{0, 0, 0};
                                for (const RwV3d& v : c.native->frustumCorners) {
                                    centroid.x += v.x * 0.125f, centroid.y += v.y * 0.125f, centroid.z += v.z * 0.125f;
                                }
                                const float span = std::fabs(c.native->farPlane - c.native->nearPlane);
                                s = {centroid, realIn(0.0f, 0.05f) * span};
                            } else {
                                const RwV3d centre = kind == 1 ? c.native->frustumCorners[rng.next() % 8u]
                                                               : RwV3d{rng.real(-6, 12), rng.real(-6, 12), rng.real(-6, 12)};
                                s = {{centre.x + rng.real(-4, 2), centre.y + rng.real(-4, 2), centre.z + rng.real(-4, 2)},
                                     rng.next() % 9u == 0 ? 0.0f : rng.real(-6, 10)};
                            }
                            g.write_float(kSphere, s.center.x), g.write_float(kSphere + 4, s.center.y);
                            g.write_float(kSphere + 8, s.center.z), g.write_float(kSphere + 12, s.radius);
                            instructions += g.call(kTestSphere.entry, {c.guest, kSphere});
                            const RwFrustumTestResult result = RwCameraFrustumTestSphere(c.native, &s);
                            require(g.r3() == static_cast<u32>(result), "RwCameraFrustumTestSphere");
                            ++sphereResults[std::clamp(static_cast<int>(result), 0, 2)];
                            ++spheres;
                        }
                    }
                } else {                                                                        // read a camera
                    std::vector<u8> data;
                    const auto put = [&](u32 w) { for (int i = 0; i < 4; ++i) data.push_back(static_cast<u8>(w >> (8 * i))); };
                    const u32 length = rng.next() % 4u == 0 ? 4u * (rng.next() % 8u) : 32u;   // short structs too
                    const u32 version = rng.next() % 10u == 0 ? 0x34000u : 0x36003u;
                    const u32 id = (((version - 0x30000u) << 14) & 0xFFC00000u) | ((version & 0x3Fu) << 16) | 0x37u;
                    put(rwID_STRUCT), put(length), put(id);
                    const RwV2d w = window();
                    const u32 proj = rng.next() % 5u == 0 ? 7u : 1u + rng.next() % 2u;
                    const u32 fields[8] = {std::bit_cast<u32>(w.x), std::bit_cast<u32>(w.y), std::bit_cast<u32>(rng.real(-2, 2)),
                                           std::bit_cast<u32>(rng.real(-2, 2)), std::bit_cast<u32>(realIn(0.01f, 5.0f)),
                                           std::bit_cast<u32>(realIn(10.0f, 5000.0f)), std::bit_cast<u32>(realIn(1, 100)), proj};
                    for (u32 i = 0; i < length / 4; ++i) put(fields[i]);
                    put(rwID_EXTENSION), put(0), put(id);
                    m.write_block(kData, data.data(), data.size());
                    m.write32(kMem, kData), m.write32(kMem + 4, static_cast<u32>(data.size()));
                    RwMemory memory{data.data(), data.size()};
                    instructions += g.call(kStreamOpen.entry, {rwSTREAMMEMORY, rwSTREAMREAD, kMem});
                    const u32 gs = g.r3();
                    RwStream* ns = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &memory);
                    instructions += g.call(kStreamRead.entry, {gs});
                    const u32 gc = g.r3();
                    RwCamera* nc = RwCameraStreamRead(ns);
                    require((gc == 0) == (nc == nullptr), "RwCameraStreamRead succeeded on one side only");
                    sameCallbacks("RwCameraStreamRead's callbacks");
                    instructions += g.call(kStreamClose.entry, {gs, 0});
                    RwStreamClose(ns, nullptr);
                    if (nc != nullptr) {
                        Cam rc{gc, nc, 0, 0, nullptr, nullptr};
                        const int cid = next_id++;
                        guest_ids[gc] = cid, g_native_ids[nc] = cid;
                        if (cams.size() < kMaxCameras) {
                            newFramePair(rc);
                            instructions += g.call(kSetFrame.entry, {gc, rc.gframe});
                            RwCameraSetFrame(nc, rc.nframe);
                            cams.push_back(rc);
                        } else {   // compare it, then destroy it on both sides
                            cams.push_back(rc);
                            sameAll("a camera read from a stream");
                            cams.pop_back();
                            instructions += g.call(kCamDestroy.entry, {gc});
                            RwCameraDestroy(nc);
                            sameCallbacks("RwCameraDestroy's callbacks");
                            guest_ids.erase(gc), g_native_ids.erase(nc);
                        }
                        ++streamsRead;
                    }
                }
            }
            sameAll("after operation " + std::to_string(op) + " of step " + std::to_string(step));
            ++checks;
        }

        std::printf("  frustums compared %u (parallel %u), sphere tests %u (outside %u, boundary %u, inside %u), cameras read %u\n",
                    framesCompared, parallelCompared, spheres, sphereResults[0], sphereResults[1], sphereResults[2], streamsRead);
        std::printf("  invalid projections %u, refused begin-updates %u, words equal only as NaN %u\n", projectionErrors, refused,
                    g_nan_words);
        require(framesCompared > 20000 && parallelCompared > 5000 && spheres > 10000 && streamsRead > 100 &&
                    sphereResults[0] > 1000 && sphereResults[1] > 1000 && sphereResults[2] > 1000 && projectionErrors > 50 &&
                    refused > 10,
                "the sessions must reach every case");
        std::printf("rwcore_camera_tests: %u operations identical to the original (%llu original instructions)\n", checks,
                    static_cast<unsigned long long>(instructions));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_camera_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
