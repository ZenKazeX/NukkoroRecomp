// Native tests of the RenderWare frames (rwcore/src/frame.cpp) for what the oracle cannot cover:
// - depth: a 100,000-frame chain (the original recurses and would exhaust its stack) built, synced, checked, destroyed;
//   a 50,000-wide hierarchy;
// - the deliberate differences of rw/core/frame.h: destroying a frame with children or objects, a hierarchy below its
//   root, cycles refused, a parentless remove, a failing plugin constructor, a clone failing at every allocation (no
//   leak, sources restored), frames without an open frame module;
// - independent math: every LTM of random hierarchies against the product of the modelling matrices up the chain in
//   double precision; the two sync paths (RwFrameGetLTM, _rwFrameSyncDirty) give the same bits;
// - the dirty-list invariant after every operation: a frame is in the dirty list exactly when its sync bits are set,
//   once, and every frame with pending subtree bits has its root (or a dirty ancestor list entry) there.
// Memory is counted: every test ends with no live allocation.
//
// Usage: rwcore_frame_native_tests
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/frame.h"
#include "rw/core/memory.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::map<void*, std::size_t> g_live;
long g_calls = 0, g_fail_at = -1;
void* countMalloc(std::size_t size, RwUInt32) {
    if (g_calls++ == g_fail_at) return nullptr;
    void* p = std::malloc(size ? size : 1);
    g_live[p] = size;
    return p;
}
void countFree(void* p) {
    if (p == nullptr) return;
    require(g_live.erase(p) == 1, "free of a pointer the allocator did not hand out");
    std::free(p);
}
void* countRealloc(void* p, std::size_t size, RwUInt32 hint) {
    if (p == nullptr) return countMalloc(size, hint);
    void* q = std::realloc(p, size);
    if (q != nullptr) g_live.erase(p), g_live[q] = size;
    return q;
}
void* countCalloc(std::size_t n, std::size_t size, RwUInt32 hint) {
    void* p = countMalloc(n * size, hint);
    if (p != nullptr) std::memset(p, 0, n * size);
    return p;
}
const RwMemoryFunctions kCounting{countMalloc, countFree, countRealloc, countCalloc};

RwError takeError() {
    RwError e{};
    RwErrorGet(&e);
    return e;
}

int g_plugin_constructs = 0, g_plugin_destructs = 0, g_fail_construct_at = -1;
void* pluginCtor(void* object, RwInt32 offset, RwInt32) {
    if (g_plugin_constructs++ == g_fail_construct_at) return nullptr;
    *reinterpret_cast<RwUInt32*>(static_cast<char*>(object) + offset) = 0xC0FFEEu;
    return object;
}
void* pluginDtor(void* object, RwInt32 offset, RwInt32) {
    ++g_plugin_destructs;
    require(*reinterpret_cast<RwUInt32*>(static_cast<char*>(object) + offset) == 0xC0FFEEu,
            "a destructor ran on plugin data its constructor never wrote");
    *reinterpret_cast<RwUInt32*>(static_cast<char*>(object) + offset) = 0xDEADu;
    return object;
}
void* pluginCopy(void* dst, const void*, RwInt32, RwInt32) { return dst; }

void startEngine() {
    RwEngineOpenParams params{nullptr};
    require(RwEngineInit(&kCounting, 0, 0) == 1, "init");
    require(RwFrameRegisterPlugin(4, 0x7002, pluginCtor, pluginDtor, pluginCopy) >= 0, "a frame plugin");
    require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "start");
}
void stopEngine(const char* what) {
    require(RwEngineStop() == 1 && RwEngineClose() == 1 && RwEngineTerm() == 1, std::string(what) + ": shutdown");
    require(g_live.empty(), std::string(what) + ": " + std::to_string(g_live.size()) + " allocations leaked");
}

RwFrame* parentOf(const RwFrame* f) { return static_cast<RwFrame*>(f->object.parent); }

/// Every frame of a forest, by walking from the given roots.
std::vector<RwFrame*> allFrames(const std::vector<RwFrame*>& roots) {
    std::vector<RwFrame*> out;
    for (RwFrame* r : roots) {
        std::vector<RwFrame*> todo{r};
        while (!todo.empty()) {
            RwFrame* f = todo.back();
            todo.pop_back();
            out.push_back(f);
            for (RwFrame* c = f->child; c != nullptr; c = c->next) todo.push_back(c);
        }
    }
    return out;
}

/// The dirty-list invariant: the list holds exactly the frames with sync bits set, each once; every root with
/// pending subtree bits below it is among them.
void checkDirty(const std::vector<RwFrame*>& frames, const char* what) {
    std::set<RwFrame*> listed;
    const RwLinkList& list = rwSrcGlobals().dirtyFrameList;
    for (const RwLLLink* l = list.link.next; l != &list.link; l = l->next) {
        auto* f = reinterpret_cast<RwFrame*>(const_cast<char*>(reinterpret_cast<const char*>(l)) - offsetof(RwFrame, inDirtyListLink));
        require(listed.insert(f).second, std::string(what) + ": a frame twice in the dirty list");
    }
    for (RwFrame* f : frames) {
        require(((f->object.privateFlags & 3) != 0) == (listed.count(f) != 0), std::string(what) + ": sync bits vs the list");
        if (f->object.privateFlags & 0x0C) {
            require(listed.count(f->root) != 0, std::string(what) + ": a frame's pending update without its root listed");
        }
        require(f->root == (parentOf(f) != nullptr ? parentOf(f)->root : f), std::string(what) + ": root field");
    }
}

/// A frame's LTM in double precision: the modelling matrices multiplied up the chain.
struct DM {
    double m[4][3];
};
DM toD(const RwMatrix& a) {
    return {{{a.right.x, a.right.y, a.right.z}, {a.up.x, a.up.y, a.up.z}, {a.at.x, a.at.y, a.at.z}, {a.pos.x, a.pos.y, a.pos.z}}};
}
DM mul(const DM& a, const DM& b) {   // row vectors: a then b
    DM r{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + (i == 3 ? b.m[3][j] : 0.0);
        }
    }
    return r;
}
DM chainLTM(const RwFrame* f) {
    DM ltm = toD(f->modelling);
    for (const RwFrame* p = parentOf(f); p != nullptr; p = parentOf(p)) ltm = mul(ltm, toD(p->modelling));
    return ltm;
}

struct Rng {
    std::uint64_t s = 0x1234567887654321ull;
    std::uint32_t next() {
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        return static_cast<std::uint32_t>(s >> 16);
    }
    float uniform(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(next() & 0xFFFFFF) / 16777216.0f; }
};

}  // namespace

int main() {
    try {
        unsigned checks = 0;

        // ---- no frame module: frames are refused, not built on an empty slot ------------------------------------
        require(RwFrameCreate() == nullptr && takeError().errorCode == static_cast<RwInt32>(0x80000001u),
                "RwFrameCreate before any engine: E_RW_BADENGINESTATE");
        ++checks;

        // ---- a 100,000-frame chain ----------------------------------------------------------------------------------
        {
            startEngine();
            constexpr int kDepth = 100000;
            RwFrame* top = RwFrameCreate();
            RwFrame* bottom = top;
            const RwV3d step{1.0f, 0.0f, 0.0f};
            for (int i = 1; i < kDepth; ++i) {
                RwFrame* f = RwFrameCreate();
                require(f != nullptr, "a chain frame");
                RwFrameTranslate(f, &step, rwCOMBINEREPLACE);
                require(RwFrameAddChild(bottom, f) == bottom, "growing the chain");
                bottom = f;
            }
            require(RwFrameGetRoot(bottom) == top, "the chain's root");
            const RwMatrix* ltm = RwFrameGetLTM(bottom);
            require(ltm->pos.x == static_cast<float>(kDepth - 1) && ltm->pos.y == 0.0f && ltm->right.x == 1.0f,
                    "the deepest LTM: the translations summed (exact in single precision)");
            require(_rwFrameSyncDirty() == 1 && RwFrameDirty(bottom) == 0, "the dirty sync of a deep chain");
            // reparent the whole chain under a new root, translated: every LTM moves
            RwFrame* base = RwFrameCreate();
            const RwV3d up{0.0f, 5.0f, 0.0f};
            RwFrameTranslate(base, &up, rwCOMBINEREPLACE);
            require(RwFrameAddChild(base, top) == base && RwFrameGetRoot(bottom) == base, "reparenting a deep chain");
            require(RwFrameGetLTM(bottom)->pos.y == 5.0f, "a deep chain follows its new root");
            require(RwFrameAddChild(bottom, base) == nullptr && takeError().errorCode == E_RW_BADPARAM,
                    "a 100,000-deep cycle refused");
            require(RwFrameDestroyHierarchy(base) == 1, "destroying a deep chain");
            stopEngine("a deep chain");
            checks += 7;
        }

        // ---- a 50,000-wide hierarchy ----------------------------------------------------------------------------------
        {
            startEngine();
            RwFrame* root = RwFrameCreate();
            std::vector<RwFrame*> kids;
            for (int i = 0; i < 50000; ++i) {
                RwFrame* f = RwFrameCreate();
                RwFrameAddChild(root, f);
                kids.push_back(f);
            }
            int seen = 0;
            RwFrameForAllChildren(root, [](RwFrame* f, void* data) -> RwFrame* { ++*static_cast<int*>(data); return f; }, &seen);
            require(seen == 50000, "every child visited");
            require(RwFrameRemoveChild(kids[0]) == kids[0] && parentOf(kids[0]) == nullptr && kids[0]->root == kids[0],
                    "removing the last child of 50,000");
            RwFrameDestroy(kids[0]);
            require(RwFrameDestroyHierarchy(root) == 1, "destroying a wide hierarchy");
            stopEngine("a wide hierarchy");
            checks += 3;
        }

        // ---- the deliberate differences --------------------------------------------------------------------------------
        {
            startEngine();
            // destroying a frame with children: they become roots, dirty, and are safe to use
            RwFrame* a = RwFrameCreate();
            RwFrame* b = RwFrameCreate();
            RwFrame* c = RwFrameCreate();
            RwFrameAddChild(a, b), RwFrameAddChild(b, c);
            const RwV3d t{2.0f, 0.0f, 0.0f};
            RwFrameTranslate(b, &t, rwCOMBINEREPLACE);
            _rwFrameSyncDirty();
            RwFrameDestroy(a);
            require(parentOf(b) == nullptr && b->root == b && c->root == b && b->next == nullptr && RwFrameDirty(b),
                    "a destroyed frame's child becomes a root, marked for an update");
            require(RwFrameGetLTM(c)->pos.x == 2.0f, "and its subtree's LTMs follow");
            checkDirty(allFrames({b}), "after destroying a parent");

            // an object attached to a frame destroyed alone loses it too
            RwObjectHasFrame lone{};
            lone.sync = [](RwObjectHasFrame* x) { return x; };
            RwFrame* single = RwFrameCreate();
            _rwObjectHasFrameSetFrame(&lone, single);
            RwFrameDestroy(single);
            require(lone.object.parent == nullptr, "an object loses a frame destroyed with RwFrameDestroy");

            // objects attached to a destroyed frame lose it
            RwObjectHasFrame o{};
            o.sync = [](RwObjectHasFrame* x) { return x; };
            _rwObjectHasFrameSetFrame(&o, c);
            RwFrameDestroyHierarchy(b);
            require(o.object.parent == nullptr, "an attached object loses its destroyed frame");
            RwFrame* d = RwFrameCreate();
            _rwObjectHasFrameSetFrame(&o, d);   // writes into the destroyed frame's list if the link was kept
            require(o.object.parent == d && d->objectList.link.next == &o.lFrame, "re-attaching after a destroy");
            _rwObjectHasFrameSetFrame(&o, nullptr);

            // destroying a hierarchy below its root detaches it first
            RwFrame* e = RwFrameCreate();
            RwFrame* f = RwFrameCreate();
            RwFrame* g = RwFrameCreate();
            RwFrameAddChild(e, f), RwFrameAddChild(f, g), RwFrameAddChild(d, e);
            RwFrameDestroyHierarchy(f);
            require(e->child == nullptr, "the parent forgets a destroyed sub-hierarchy");
            checkDirty(allFrames({d}), "after destroying a sub-hierarchy");

            // cycles and a parentless remove
            RwFrame* h = RwFrameCreate();
            RwFrameAddChild(e, h);
            require(RwFrameAddChild(h, d) == nullptr && takeError().errorCode == E_RW_BADPARAM && parentOf(d) == nullptr,
                    "an ancestor cannot become a child");
            require(RwFrameAddChild(h, h) == nullptr && takeError().errorCode == E_RW_BADPARAM, "a frame cannot be its own child");
            require(RwFrameRemoveChild(d) == d && parentOf(d) == nullptr, "removing a root changes nothing");
            checkDirty(allFrames({d}), "after refused operations");
            RwFrameDestroyHierarchy(d);

            // a failing plugin constructor fails the creation
            g_fail_construct_at = g_plugin_constructs;
            require(RwFrameCreate() == nullptr, "a frame whose plugin constructor fails");
            g_fail_construct_at = -1;
            stopEngine("the differences");
            checks += 11;
        }

        // ---- a sync callback that walks another hierarchy (RwFrameGetLTM) in the middle of the dirty sync -----------
        {
            startEngine();
            static RwFrame* other = nullptr;
            static std::vector<RwMatrix> seenFromSync;
            // A: a chain a[0..10], then a[11..79] all children of a[10] (a fan of siblings); B: a chain
            std::vector<RwFrame*> a, b;
            for (int i = 0; i < 80; ++i) {
                a.push_back(RwFrameCreate()), b.push_back(RwFrameCreate());
                if (i > 0) RwFrameAddChild(a[i <= 10 ? i - 1 : 10], a[i]), RwFrameAddChild(b[i - 1], b[i]);
                const RwV3d v{0.5f, static_cast<float>(i % 4), 0.25f};
                RwFrameTranslate(a[i], &v, rwCOMBINEREPLACE), RwFrameRotate(b[i], &v, 7.0f, rwCOMBINEREPLACE);
            }
            _rwFrameSyncDirty();   // both clean; then B dirty, then a frame inside A (A joins the list later: first)
            const RwV3d nudge{0.0f, 0.0f, 3.0f};
            RwFrameTranslate(b[5], &nudge, rwCOMBINEPOSTCONCAT);
            RwFrameTranslate(a[10], &nudge, rwCOMBINEPOSTCONCAT);
            // the watcher on a[10]'s first child walks B (still dirty) mid-way through A's walk: if the walks shared
            // their flag stack, a[10]'s other children would lose its pending-LTM bit and keep stale LTMs
            other = b.back();
            RwObjectHasFrame watcher{};
            watcher.sync = [](RwObjectHasFrame* o) {
                seenFromSync.push_back(*RwFrameGetLTM(other));   // a nested walk of another dirty hierarchy
                return o;
            };
            _rwObjectHasFrameSetFrame(&watcher, a[79]);   // a[10]'s first child (children are added at the head)
            _rwFrameSyncDirty();
            require(!seenFromSync.empty(), "the watcher ran");
            require(std::memcmp(&seenFromSync.back(), &other->ltm, 48) == 0, "the nested walk saw the final LTM");
            std::vector<RwFrame*> all = allFrames({a[0], b[0]});
            for (RwFrame* x : all) {
                const DM e = chainLTM(x);
                const float* g = &x->ltm.right.x;
                for (int r = 0; r < 4; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        require(std::fabs(g[r * 4 + c] - e.m[r][c]) <= 1e-3 * std::fmax(1.0, std::fabs(e.m[r][c])),
                                "an LTM after a sync with a nested walk");
                    }
                }
            }
            checkDirty(all, "after a sync with a nested walk");
            _rwObjectHasFrameSetFrame(&watcher, nullptr);
            RwFrameDestroyHierarchy(a[0]), RwFrameDestroyHierarchy(b[0]);
            stopEngine("a nested walk");
            checks += 2;
        }

        // ---- a clone failing at every allocation ----------------------------------------------------------------------
        {
            startEngine();
            Rng rng;
            std::vector<RwFrame*> nodes{RwFrameCreate()};
            for (int i = 1; i < 300; ++i) {   // 300 frames: more than the 50 per free-list block
                RwFrame* n = RwFrameCreate();
                RwFrameAddChild(nodes[rng.next() % nodes.size()], n);
                nodes.push_back(n);
            }
            RwFrame* root = nodes[0];
            _rwFrameSyncDirty();
            bool succeeded = false;
            for (long k = 0; !succeeded && k < 100; ++k) {
                const auto before = g_live.size();
                const int constructs = g_plugin_constructs, destructs = g_plugin_destructs;
                g_calls = 0, g_fail_at = k;
                RwFrame* copy = _rwFrameCloneAndLinkClones(root);
                g_fail_at = -1;
                if (copy == nullptr) {
                    require(g_live.size() == before, "a failed clone leaks (k=" + std::to_string(k) + ")");
                    require(g_plugin_constructs - constructs == g_plugin_destructs - destructs,
                            "a failed clone destructs exactly what it constructed");
                    checkDirty(allFrames({root}), "the sources after a failed clone (roots restored)");
                } else {
                    succeeded = true;
                    require(allFrames({copy}).size() == nodes.size(), "the copy has every frame");
                    _rwFramePurgeClone(root);
                    checkDirty(allFrames({root, copy}), "after a clone and purge");
                    RwFrameDestroyHierarchy(copy);
                }
                ++checks;
            }
            require(succeeded, "a clone eventually succeeds");
            RwFrameDestroyHierarchy(root);
            stopEngine("failing clones");
        }

        // ---- independent math and the dirty invariant over random hierarchies --------------------------------------
        {
            startEngine();
            Rng rng;
            std::vector<RwFrame*> frames;
            for (int i = 0; i < 200; ++i) frames.push_back(RwFrameCreate());
            double worst = 0;
            for (int step = 0; step < 20000; ++step) {
                RwFrame* f = frames[rng.next() % frames.size()];
                const unsigned op = rng.next() % 10u;
                if (op < 3) {
                    RwFrame* p = frames[rng.next() % frames.size()];
                    if (RwFrameAddChild(p, f) == nullptr) takeError();
                } else if (op < 4) {
                    RwFrameRemoveChild(f);
                } else if (op < 7) {
                    const RwV3d v{rng.uniform(-2, 2), rng.uniform(-2, 2), rng.uniform(-2, 2)};
                    switch (rng.next() % 3u) {
                        case 0: RwFrameTranslate(f, &v, static_cast<RwOpCombineType>(rng.next() % 3u)); break;
                        case 1: RwFrameRotate(f, &v, rng.uniform(-180, 180), static_cast<RwOpCombineType>(rng.next() % 3u)); break;
                        default: {
                            const RwV3d s{rng.uniform(0.5f, 1.5f), rng.uniform(0.5f, 1.5f), rng.uniform(0.5f, 1.5f)};
                            RwFrameScale(f, &s, rwCOMBINEPOSTCONCAT);
                        }
                    }
                    if (rng.next() % 50u == 0) RwFrameSetIdentity(f);
                } else if (op < 9) {
                    // the two sync paths agree bit for bit: LTMs through GetLTM, then the dirty sync must not change them
                    std::vector<RwMatrix> viaGet;
                    for (RwFrame* x : frames) viaGet.push_back(*RwFrameGetLTM(x));
                    _rwFrameSyncDirty();
                    for (std::size_t i = 0; i < frames.size(); ++i) {
                        require(std::memcmp(&viaGet[i], &frames[i]->ltm, 48) == 0, "GetLTM and the dirty sync disagree");
                    }
                    for (RwFrame* x : frames) {   // against the double-precision chain
                        const DM e = chainLTM(x);
                        const float* g = &x->ltm.right.x;
                        for (int r = 0; r < 4; ++r) {
                            for (int c = 0; c < 3; ++c) {
                                const double scale = std::fmax(1.0, std::fabs(e.m[r][c]));
                                worst = std::fmax(worst, std::fabs(g[r * 4 + c] - e.m[r][c]) / scale);
                            }
                        }
                    }
                } else {
                    _rwFrameSyncDirty();
                }
                checkDirty(frames, "a random operation");
                ++checks;
            }
            std::printf("  LTM vs double-precision chain: worst relative error %.3g\n", worst);
            require(worst < 1e-3, "LTMs match the double-precision product of the chain");
            std::vector<RwFrame*> roots;
            for (RwFrame* x : frames) if (parentOf(x) == nullptr) roots.push_back(x);
            for (RwFrame* r : roots) RwFrameDestroyHierarchy(r);
            stopEngine("random hierarchies");
        }

        std::printf("rwcore_frame_native_tests: %u checks passed (no allocation outlives a test)\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_frame_native_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
