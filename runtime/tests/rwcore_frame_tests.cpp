// Oracle for the native RenderWare frames (rwcore/src/frame.cpp): the ORIGINAL baframe / basync / batypehf functions
// of PAL main.dol in the reference interpreter (rwcore_oracle.h) and the native ones run the same random sessions on
// a forest of frames: create, destroy (frames without children), destroy whole hierarchies (roots), add / add without
// update / remove children (chains deeper than the original's inlined recursion levels included), the four
// transforms and set-identity, update-objects, get-LTM, the dirty sync with attached objects, clone-and-link and
// purge, for-all-children with a callback that stops early, a frame plugin. After every operation: every live
// frame's object bytes, parent / child / next / root (as identities), modelling matrix (rows and flags), LTM (rows and
// flags, wherever its hierarchy has no pending LTM update - the original leaves a new copy's LTM unset until then),
// object lists in order, the dirty list in order, RwFrameDirty / RwFrameGetRoot, every returned matrix, every
// plugin / sync / for-all callback in order, and the held error.
//
// Cases where the native deliberately differs (rw/core/frame.h) are not generated here: destroying a frame that has
// children or objects, destroying a hierarchy below its root, cycles, removing a parentless frame, failing allocations
// or constructors. rwcore_frame_native_tests covers them.
// liveness-roots: reference-interpreter-only
//
// Usage: rwcore_frame_tests <main.dol>
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/frame.h"
#include "rw/core/plugin.h"
#include "rwcore_oracle.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
using namespace rworacle;

constexpr Original kFrameOpen{"_rwFrameOpen", 0x804878D8u, 0xA4u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kClone{"_rwFrameCloneAndLinkClones", 0x80487CACu, 0x48u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kPurge{"_rwFramePurgeClone", 0x80487CF4u, 0x4Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kDirty{"RwFrameDirty", 0x80487D40u, 0x10u, 0x806300A0u, 0x4E800020u};
constexpr Original kCreate{"RwFrameCreate", 0x80487D50u, 0x12Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kDestroy{"RwFrameDestroy", 0x80487E7Cu, 0xACu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kDestroyHier{"RwFrameDestroyHierarchy", 0x80488258u, 0x24u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kUpdate{"RwFrameUpdateObjects", 0x8048827Cu, 0x6Cu, 0x80A300A0u, 0x4E800020u};
constexpr Original kGetLTM{"RwFrameGetLTM", 0x804882E8u, 0x40u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kGetRoot{"RwFrameGetRoot", 0x80488328u, 0x8u, 0x806300A0u, 0x4E800020u};
constexpr Original kAddNoUpdate{"RwFrameAddChildNoUpdate", 0x80488330u, 0x68u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kAdd{"RwFrameAddChild", 0x80488398u, 0x100u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRemove{"RwFrameRemoveChild", 0x80488498u, 0xE4u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kForAll{"RwFrameForAllChildren", 0x8048857Cu, 0x84u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kTranslate{"RwFrameTranslate", 0x80488600u, 0x9Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kScale{"RwFrameScale", 0x8048869Cu, 0x9Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kTransform{"RwFrameTransform", 0x80488738u, 0x9Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRotate{"RwFrameRotate", 0x804887D4u, 0x9Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kIdentity{"RwFrameSetIdentity", 0x80488870u, 0xB4u, 0xC0221C00u, 0x4E800020u};
constexpr Original kRegister{"RwFrameRegisterPlugin", 0x80488924u, 0x40u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSyncDirty{"_rwFrameSyncDirty", 0x8048C3A4u, 0x6Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSyncLTM{"_rwFrameSyncHierarchyLTM", 0x8048C628u, 0xD4u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSetFrame{"_rwObjectHasFrameSetFrame", 0x8048E810u, 0x74u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRelease{"_rwObjectHasFrameReleaseFrame", 0x8048E884u, 0x28u, 0x80030004u, 0x4E800020u};
constexpr Original kMemoryOpen{"_rwMemoryOpen", 0x804812C4u, 0x118u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRegOpen{"_rwPluginRegistryOpen", 0x80482BE8u, 0x5Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kErrorOpen{"_rwErrorOpen", 0x8047EB88u, 0x38u, 0x390D6100u, 0x4E800020u};
constexpr Original kErrorGet{"RwErrorGet", 0x8047EC34u, 0x44u, 0x808D615Cu, 0x4E800020u};
constexpr Original kMatrixOpen{"_rwMatrixOpen", 0x8047F8DCu, 0xD4u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kVectorOpen{"_rwVectorOpen", 0x8048363Cu, 0x398u, 0x9421FFB0u, 0x4E800020u};
constexpr u32 kFreeListAlloc = 0x80480E00u, kFreeListFree = 0x80480FA0u;
constexpr u32 kCtorStub = 0x817F0060u, kDtorStub = 0x817F0070u, kCopyStub = 0x817F0080u, kSyncStub = 0x817F0090u,
              kForAllStub = 0x817F00A0u;
constexpr u32 kGuestErrorOffset = 0x200u, kGuestVectorOffset = 0x210u, kGuestMatrixOffset = 0x240u,
              kGuestFrameOffset = 0x280u;
constexpr u32 kFuncs = kBuffer, kErr = kBuffer + 0x20u, kVec = kBuffer + 0x40u, kMat = kBuffer + 0x80u,
              kObjects = 0x81020000u;
constexpr u32 kDirtyHead = kEngine + 0xBCu;   // RwGlobals::dirtyFrameList on the GameCube
constexpr unsigned kMaxFrames = 120, kObjectCount = 24;

// ---- what each side's callbacks saw --------------------------------------------------------------------------------
struct Seen {
    int kind;          // 0 ctor, 1 dtor, 2 copy, 3 sync, 4 for-all
    int a, b;          // identities (frame / object indices; -1 new, -2 unknown)
    bool operator==(const Seen&) const = default;
};
std::vector<Seen> g_native_seen;
std::vector<u32> g_script;
std::size_t g_native_next = 0;
u32 script(std::size_t& next) { return g_script[next++ % g_script.size()]; }

std::map<const void*, int> g_native_ids;   // native frame / object -> index
int nativeId(const void* p) {
    const auto it = g_native_ids.find(p);
    return it == g_native_ids.end() ? -1 : it->second;
}
void* nativeCtor(void* object, RwInt32, RwInt32) {
    g_native_seen.push_back({0, nativeId(object), 0});
    return object;
}
void* nativeDtor(void* object, RwInt32, RwInt32) {
    g_native_seen.push_back({1, nativeId(object), 0});
    return object;
}
void* nativeCopy(void* dst, const void* src, RwInt32, RwInt32) {
    g_native_seen.push_back({2, nativeId(dst), nativeId(src)});
    return dst;
}
RwObjectHasFrame* nativeSync(RwObjectHasFrame* object) {
    g_native_seen.push_back({3, nativeId(object), 0});
    return object;
}
RwFrame* nativeForAll(RwFrame* frame, void*) {
    g_native_seen.push_back({4, nativeId(frame), 0});
    return script(g_native_next) % 4u == 0 ? nullptr : frame;
}

struct Pair {
    u32 guest;
    RwFrame* native;
};

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rwcore_frame_tests main.dol");
        Guest g(argv[1]);
        for (const Original& f : {kFrameOpen, kClone, kPurge, kDirty, kCreate, kDestroy, kDestroyHier, kUpdate, kGetLTM,
                                  kGetRoot, kAddNoUpdate, kAdd, kRemove, kForAll, kTranslate, kScale, kTransform, kRotate,
                                  kIdentity, kRegister, kSyncDirty, kSyncLTM, kSetFrame, kRelease, kMemoryOpen, kRegOpen,
                                  kErrorOpen, kErrorGet, kMatrixOpen, kVectorOpen}) {
            g.check_pinned(f);
        }
        GuestMemory& m = g.memory();
        Rng rng;
        for (unsigned i = 0; i < 101; ++i) g_script.push_back(rng.next());

        // ---- identities of both sides ------------------------------------------------------------------------------
        std::vector<Pair> frames;   // live frames; index = identity
        std::map<u32, int> guest_ids;
        const auto guestId = [&](u32 a) {
            const auto it = guest_ids.find(a);
            return it == guest_ids.end() ? -1 : it->second;
        };
        std::vector<Seen> guest_seen;
        std::size_t guest_next = 0;
        g.stub(kCtorStub, [&](Cpu& c) { guest_seen.push_back({0, guestId(c.gpr[3]), 0}); });
        g.stub(kDtorStub, [&](Cpu& c) { guest_seen.push_back({1, guestId(c.gpr[3]), 0}); });
        g.stub(kCopyStub, [&](Cpu& c) { guest_seen.push_back({2, guestId(c.gpr[3]), guestId(c.gpr[4])}); });
        g.stub(kSyncStub, [&](Cpu& c) { guest_seen.push_back({3, guestId(c.gpr[3]), 0}); });
        g.stub(kForAllStub, [&](Cpu& c) {
            guest_seen.push_back({4, guestId(c.gpr[3]), 0});
            if (script(guest_next) % 4u == 0) c.gpr[3] = 0;
        });

        // ---- the guest engine: memory, errors, plugin registry, vector, matrix, a frame plugin, frames --------------
        g.reset_engine();
        for (u32 a : {kErrorModule, kErrorModule + 4u, kVectorModule, kVectorModule + 4u, kMatrixModule, kMatrixModule + 4u}) {
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
        m.write32(0x805F2658u, 0);   // no engine open: plugins may register
        g.call(kRegister.entry, {12, 0x7001, kCtorStub, kDtorStub, kCopyStub});
        require(static_cast<int>(g.r3()) >= 0xA4, "the guest frame plugin");
        g.call(kFrameOpen.entry, {kEngine, kGuestFrameOffset, 4});
        require(g.r3() == kEngine, "guest frames open");

        // ---- the native engine -----------------------------------------------------------------------------------
        RwEngineOpenParams params{nullptr};
        require(RwEngineInit(nullptr, 0, 0) == 1, "native init");
        require(RwFrameRegisterPlugin(12, 0x7001, nativeCtor, nativeDtor, nativeCopy) >= static_cast<RwInt32>(sizeof(RwFrame)),
                "the native frame plugin");
        require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "native start");

        // ---- attachable objects (RwObjectHasFrame), each side's own ---------------------------------------------
        std::vector<RwObjectHasFrame> native_objects(kObjectCount);
        for (unsigned i = 0; i < kObjectCount; ++i) {
            const u32 o = kObjects + i * 0x20u;
            for (u32 w = 0; w < 5; ++w) m.write32(o + 4 * w, 0);
            m.write32(o + 0x10, kSyncStub);
            native_objects[i] = {};
            native_objects[i].sync = nativeSync;
            guest_ids[o] = 1000 + static_cast<int>(i);
            g_native_ids[&native_objects[i]] = 1000 + static_cast<int>(i);
        }

        u64 instructions = 0;
        unsigned checks = 0;
        int next_id = 0;
        // coverage, reported at the end: operations by kind, LTMs compared, deepest hierarchy, syncs, clone sizes
        std::map<std::string, unsigned> done;
        unsigned ltms = 0, deepest = 0, syncs = 0, largestClone = 0;
        const auto addPair = [&](u32 gf, RwFrame* nf) {
            const int id = next_id++;
            frames.push_back({gf, nf});
            guest_ids[gf] = id;
            g_native_ids[nf] = id;
        };
        const auto idOfGuest = [&](u32 a) { return a == 0 ? -10 : guestId(a); };
        const auto idOfNative = [&](const void* p) { return p == nullptr ? -10 : nativeId(p); };
        const auto removeFrame = [&](std::size_t k) {
            guest_ids.erase(frames[k].guest);
            g_native_ids.erase(frames[k].native);
            frames.erase(frames.begin() + static_cast<long>(k));
        };
        const auto sameMatrix = [&](u32 ga, const RwMatrix& n, const char* what) {
            const auto* w = reinterpret_cast<const u32*>(&n);
            for (u32 i = 0; i < 16; ++i) {
                if (i == 7 || i == 11 || i == 15) continue;   // pads: the original leaves them as memory held them
                require(m.peek32(ga + 4 * i) == w[i], what);
            }
        };
        const auto sameCallbacks = [&](const char* what) {
            require(guest_seen == g_native_seen, what);
            guest_seen.clear(), g_native_seen.clear();
            instructions += g.call(kErrorGet.entry, {kErr});
            RwError e{};
            RwErrorGet(&e);
            require(m.peek32(kErr) == static_cast<u32>(e.pluginID) && m.peek32(kErr + 4) == static_cast<u32>(e.errorCode),
                    what);
        };
        const auto sameAll = [&](const char* what) {
            const std::string w = what;
            for (const Pair& p : frames) {
                const RwFrame& n = *p.native;
                for (u32 b = 0; b < 4; ++b) require(m.peek8(p.guest + b) == (&n.object.type)[b], w + ": object bytes");
                require(idOfGuest(m.peek32(p.guest + 4)) == idOfNative(n.object.parent) &&
                            idOfGuest(m.peek32(p.guest + 0x98)) == idOfNative(n.child) &&
                            idOfGuest(m.peek32(p.guest + 0x9C)) == idOfNative(n.next) &&
                            idOfGuest(m.peek32(p.guest + 0xA0)) == idOfNative(n.root),
                        w + ": links");
                sameMatrix(p.guest + 0x10, n.modelling, (w + ": modelling").c_str());
                if (!(n.root->object.privateFlags & rwFRAMEPRIVATEHIERARCHYSYNCLTM)) {
                    sameMatrix(p.guest + 0x50, n.ltm, (w + ": ltm").c_str());
                    ++ltms;
                }
                unsigned depth = 0;
                for (const RwFrame* f = &n; f->object.parent != nullptr; f = static_cast<const RwFrame*>(f->object.parent)) ++depth;
                deepest = std::max(deepest, depth);
                std::vector<int> go, no;   // object lists
                for (u32 l = m.peek32(p.guest + 0x90), k = 0; l != p.guest + 0x90 && k < 100; l = m.peek32(l), ++k) go.push_back(guestId(l - 8));
                for (const RwLLLink* l = n.objectList.link.next; l != &n.objectList.link; l = l->next) {
                    no.push_back(nativeId(reinterpret_cast<const char*>(l) - offsetof(RwObjectHasFrame, lFrame)));
                }
                require(go == no, w + ": object list");
            }
            std::vector<int> gd, nd;   // the dirty list
            for (u32 l = m.peek32(kDirtyHead), k = 0; l != kDirtyHead && k < 1000; l = m.peek32(l), ++k) gd.push_back(guestId(l - 8));
            for (const RwLLLink* l = rwSrcGlobals().dirtyFrameList.link.next; l != &rwSrcGlobals().dirtyFrameList.link; l = l->next) {
                nd.push_back(nativeId(reinterpret_cast<const char*>(l) - offsetof(RwFrame, inDirtyListLink)));
            }
            require(gd == nd, w + ": dirty list");
            for (unsigned i = 0; i < kObjectCount; ++i) {
                require(idOfGuest(m.peek32(kObjects + i * 0x20u + 4)) == idOfNative(native_objects[i].object.parent),
                        w + ": an object's frame");
            }
        };
        const auto pick = [&]() -> std::size_t { return rng.next() % frames.size(); };
        const auto isAncestor = [&](const RwFrame* a, const RwFrame* f) {   // a == f or above it
            for (const RwFrame* p = f; p != nullptr; p = static_cast<const RwFrame*>(p->object.parent)) {
                if (p == a) return true;
            }
            return false;
        };
        const auto hasObjects = [](const RwFrame* f) { return f->objectList.link.next != &f->objectList.link; };
        const auto subtreeHasObjects = [&](RwFrame* top) {
            std::vector<RwFrame*> todo{top};
            while (!todo.empty()) {
                RwFrame* f = todo.back();
                todo.pop_back();
                if (hasObjects(f)) return true;
                for (RwFrame* c = f->child; c != nullptr; c = c->next) todo.push_back(c);
            }
            return false;
        };

        for (unsigned round = 0; round < 60; ++round) {
            for (unsigned step = 0; step < 400; ++step) {
                const u32 op = rng.next() % 100u;
                if (frames.size() < 3 || (op < 12 && frames.size() < kMaxFrames)) {             // create
                    instructions += g.call(kCreate.entry, {});
                    const u32 gf = g.r3();
                    RwFrame* nf = RwFrameCreate();
                    require(gf != 0 && nf != nullptr, "RwFrameCreate");
                    // the constructor ran before the frame had an identity on both sides: -1
                    addPair(gf, nf);
                    sameCallbacks("RwFrameCreate's callbacks");
                } else if (op < 30) {                                                            // add a child
                    const Pair parent = frames[pick()], child = frames[pick()];
                    if (isAncestor(child.native, parent.native)) continue;   // no cycles in the oracle
                    const bool update = rng.next() % 4u != 0;
                    instructions += g.call(update ? kAdd.entry : kAddNoUpdate.entry, {parent.guest, child.guest});
                    RwFrame* r = update ? RwFrameAddChild(parent.native, child.native)
                                        : RwFrameAddChildNoUpdate(parent.native, child.native);
                    require(g.r3() == parent.guest && r == parent.native, "RwFrameAddChild's result");
                    sameCallbacks("RwFrameAddChild's callbacks");
                } else if (op < 38) {                                                            // remove a child
                    const Pair p = frames[pick()];
                    if (p.native->object.parent == nullptr) continue;
                    instructions += g.call(kRemove.entry, {p.guest});
                    require(g.r3() == p.guest && RwFrameRemoveChild(p.native) == p.native, "RwFrameRemoveChild's result");
                } else if (op < 52) {                                                            // transform
                    const Pair p = frames[pick()];
                    const u32 kind = rng.next() % 5u;
                    const RwOpCombineType combine = static_cast<RwOpCombineType>(rng.next() % 3u);
                    const RwV3d v{rng.real(-3, 3), rng.real(-3, 3), rng.real(-3, 3)};
                    g.write_float(kVec, v.x), g.write_float(kVec + 4, v.y), g.write_float(kVec + 8, v.z);
                    RwMatrix t{};
                    for (u32 i = 0; i < 16; ++i) {
                        const float f = (i == 3 || i == 7 || i == 11 || i == 15) ? 0.0f : rng.real(-2, 2);
                        reinterpret_cast<float*>(&t)[i] = f;
                        g.write_float(kMat + 4 * i, f);
                    }
                    t.flags = 0, m.write32(kMat + 12, 0);
                    const float angle = rng.real(-4, 8);
                    RwFrame* r = nullptr;
                    switch (kind) {
                        case 0: instructions += g.call(kTranslate.entry, {p.guest, kVec, static_cast<u32>(combine)});
                                r = RwFrameTranslate(p.native, &v, combine); break;
                        case 1: instructions += g.call(kScale.entry, {p.guest, kVec, static_cast<u32>(combine)});
                                r = RwFrameScale(p.native, &v, combine); break;
                        case 2: instructions += g.call(kTransform.entry, {p.guest, kMat, static_cast<u32>(combine)});
                                r = RwFrameTransform(p.native, &t, combine); break;
                        case 3: instructions += g.call(kRotate.entry, {p.guest, kVec, static_cast<u32>(combine)}, {double(angle)});
                                r = RwFrameRotate(p.native, &v, angle, combine); break;
                        default:
                            if (rng.next() % 2u == 0) {   // flag bits set-identity must keep (none of the sessions' own)
                                const u32 extra = (rng.next() & 0xFFFCFFFCu) | (p.native->modelling.flags & 0x20003u);
                                p.native->modelling.flags = extra, m.write32(p.guest + 0x1C, extra);
                            }
                            instructions += g.call(kIdentity.entry, {p.guest});
                                 r = RwFrameSetIdentity(p.native); break;
                    }
                    require(g.r3() == p.guest && r == p.native, "a frame transform's result");
                    sameCallbacks("a transform's error");
                } else if (op < 56) {                                                            // update objects
                    const Pair p = frames[pick()];
                    instructions += g.call(kUpdate.entry, {p.guest});
                    require(g.r3() == p.guest && RwFrameUpdateObjects(p.native) == p.native, "RwFrameUpdateObjects");
                } else if (op < 66) {                                                            // get the LTM
                    const Pair p = frames[pick()];
                    instructions += g.call(kGetLTM.entry, {p.guest});
                    RwMatrix* ltm = RwFrameGetLTM(p.native);
                    require(g.r3() == p.guest + 0x50 && ltm == &p.native->ltm, "RwFrameGetLTM's result");
                    sameMatrix(g.r3(), *ltm, "RwFrameGetLTM's matrix");
                    instructions += g.call(kDirty.entry, {p.guest});
                    require(g.r3() == static_cast<u32>(RwFrameDirty(p.native)), "RwFrameDirty");
                    instructions += g.call(kGetRoot.entry, {p.guest});
                    require(idOfGuest(g.r3()) == idOfNative(RwFrameGetRoot(p.native)), "RwFrameGetRoot");
                } else if (op < 72) {                                                            // the dirty sync
                    instructions += g.call(kSyncDirty.entry, {});
                    require(g.r3() == 1 && _rwFrameSyncDirty() == 1, "_rwFrameSyncDirty's result");
                    syncs += static_cast<unsigned>(std::count_if(g_native_seen.begin(), g_native_seen.end(), [](const Seen& s) { return s.kind == 3; }));
                    sameCallbacks("_rwFrameSyncDirty's object syncs");
                } else if (op < 77) {                                                            // the LTM sync alone
                    const Pair p = frames[pick()];
                    RwFrame* root = p.native->root;
                    const u32 groot = m.peek32(p.guest + 0xA0);
                    instructions += g.call(kSyncLTM.entry, {groot});
                    _rwFrameSyncHierarchyLTM(root);
                } else if (op < 84) {                                                            // attach / detach
                    const unsigned o = rng.next() % kObjectCount;
                    const u32 go = kObjects + o * 0x20u;
                    if (rng.next() % 3u == 0) {
                        instructions += g.call(kRelease.entry, {go});
                        _rwObjectHasFrameReleaseFrame(&native_objects[o]);
                        // the original keeps object->parent: detach it fully before the next attach
                        m.write32(go + 4, 0), native_objects[o].object.parent = nullptr;
                    } else {
                        const bool none = rng.next() % 6u == 0;
                        const Pair p = frames[pick()];
                        instructions += g.call(kSetFrame.entry, {go, none ? 0u : p.guest});
                        _rwObjectHasFrameSetFrame(&native_objects[o], none ? nullptr : p.native);
                    }
                } else if (op < 88) {                                                            // for all children
                    const Pair p = frames[pick()];
                    instructions += g.call(kForAll.entry, {p.guest, kForAllStub, 0x1234});
                    require(g.r3() == p.guest && RwFrameForAllChildren(p.native, nativeForAll, nullptr) == p.native,
                            "RwFrameForAllChildren's result");
                    sameCallbacks("RwFrameForAllChildren's callbacks");
                } else if (op < 92) {                                                            // destroy a leaf
                    const std::size_t k = pick();
                    const Pair p = frames[k];
                    if (p.native->child != nullptr || hasObjects(p.native)) continue;
                    instructions += g.call(kDestroy.entry, {p.guest});
                    require(g.r3() == 1 && RwFrameDestroy(p.native) == 1, "RwFrameDestroy's result");
                    sameCallbacks("RwFrameDestroy's callbacks");
                    removeFrame(k);
                } else if (op < 95) {                                                            // destroy a hierarchy
                    const std::size_t k = pick();
                    const Pair p = frames[k];
                    if (p.native->object.parent != nullptr || subtreeHasObjects(p.native) || frames.size() < 6) continue;
                    // the identities of the whole hierarchy, removed after the call (callbacks name them)
                    std::vector<RwFrame*> doomed{p.native};
                    for (std::size_t i = 0; i < doomed.size(); ++i) {
                        for (RwFrame* c = doomed[i]->child; c != nullptr; c = c->next) doomed.push_back(c);
                    }
                    instructions += g.call(kDestroyHier.entry, {p.guest});
                    require(g.r3() == 1 && RwFrameDestroyHierarchy(p.native) == 1, "RwFrameDestroyHierarchy's result");
                    sameCallbacks("RwFrameDestroyHierarchy's callbacks (post-order)");
                    for (RwFrame* d : doomed) {
                        const auto it = std::find_if(frames.begin(), frames.end(), [&](const Pair& q) { return q.native == d; });
                        removeFrame(static_cast<std::size_t>(it - frames.begin()));
                    }
                } else if (frames.size() < kMaxFrames - 30) {                                    // clone a hierarchy
                    const Pair p = frames[pick()];
                    RwFrame* root = p.native->root;
                    const u32 groot = m.peek32(p.guest + 0xA0);
                    // a frame below the root still carrying "in the dirty list" bits (left by AddChildNoUpdate) is a
                    // case where the copies differ on purpose (rw/core/frame.h): not cloned here
                    bool marked = false;
                    for (RwFrame* f = root->child; f != nullptr && !marked;) {
                        marked = (f->object.privateFlags & 3) != 0;
                        if (f->child != nullptr) { f = f->child; continue; }
                        while (f != root && f->next == nullptr) f = static_cast<RwFrame*>(f->object.parent);
                        f = f == root ? nullptr : f->next;
                    }
                    if (marked) continue;
                    instructions += g.call(kClone.entry, {groot});
                    const u32 gclone = g.r3();
                    RwFrame* nclone = _rwFrameCloneAndLinkClones(root);
                    require(gclone != 0 && nclone != nullptr, "_rwFrameCloneAndLinkClones");
                    // map the copies by walking both copies the same way (children in list order)
                    std::vector<std::pair<u32, RwFrame*>> todo{{gclone, nclone}};
                    while (!todo.empty()) {
                        const auto [gc, nc] = todo.back();
                        todo.pop_back();
                        addPair(gc, nc);
                        u32 gch = m.peek32(gc + 0x98);
                        RwFrame* nch = nc->child;
                        for (; gch != 0 && nch != nullptr; gch = m.peek32(gch + 0x9C), nch = nch->next) todo.push_back({gch, nch});
                        require(gch == 0 && nch == nullptr, "a copy's children differ in number");
                    }
                    largestClone = std::max(largestClone, static_cast<unsigned>(frames.size()));
                    // the copies' constructors ran before they had identities: the log names them -1 on both sides
                    sameCallbacks("_rwFrameCloneAndLinkClones' callbacks");
                    if (rng.next() % 3u != 0) {
                        sameAll("after a clone, before the purge");
                    }
                    instructions += g.call(kPurge.entry, {groot});
                    require(g.r3() == groot && _rwFramePurgeClone(root) == root, "_rwFramePurgeClone");
                }
                sameAll("after an operation");
                ++checks;
                ++done[std::to_string(op < 12 ? 0 : op < 30 ? 1 : op < 38 ? 2 : op < 52 ? 3 : op < 56 ? 4 : op < 66 ? 5 : op < 72 ? 6 : op < 77 ? 7 : op < 84 ? 8 : op < 88 ? 9 : op < 92 ? 10 : op < 95 ? 11 : 12)];
            }
        }

        std::printf("  coverage (op: count) create/add/remove/transform/update/getLTM/syncDirty/syncLTM/attach/forAll/"
                    "destroy/destroyHierarchy/clone:");
        for (const auto& [k, v] : done) std::printf(" %s:%u", k.c_str(), v);
        std::printf("\n  LTMs compared %u, deepest hierarchy %u, object syncs %u, frames after the largest clone %u\n", ltms,
                    deepest, syncs, largestClone);
        require(deepest > 8 && syncs > 1000 && ltms > 100000, "the sessions must reach deep hierarchies, syncs and LTMs");
        std::printf("rwcore_frame_tests: %u operations identical to the original (%llu original instructions)\n",
                    checks, static_cast<unsigned long long>(instructions));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_frame_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
