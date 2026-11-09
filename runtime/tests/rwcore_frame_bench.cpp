// Benchmark of the native RenderWare frames (rwcore/src/frame.cpp) on the workloads a stage puts on them. Not a test
// (no pass / fail): median of 7 runs per workload, in milliseconds, for comparing builds of the frame code.
//   create/destroy: 200,000 frames created, then destroyed
//   build:          1,000 hierarchies of 64 frames, random shapes, built with RwFrameAddChild
//   sync:           every leaf of those hierarchies moved, then one _rwFrameSyncDirty (64,000 LTMs, object syncs)
//   deep LTM:       the root of a 10,000-frame chain moved, then RwFrameGetLTM of its deepest frame
//
// Usage: rwcore_frame_bench
#include "rw/core/engine.h"
#include "rw/core/frame.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

struct Rng {
    std::uint64_t s = 0x5DEECE66Dull;
    std::uint32_t next() {
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        return static_cast<std::uint32_t>(s >> 16);
    }
};

template <class F> double medianMs(F run) {
    std::vector<double> t;
    for (int i = 0; i < 7; ++i) {
        const auto a = std::chrono::steady_clock::now();
        run();
        t.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
    }
    std::sort(t.begin(), t.end());
    return t[3];
}

RwObjectHasFrame* syncNothing(RwObjectHasFrame* o) { return o; }

}  // namespace

int main() {
    RwEngineOpenParams params{nullptr};
    if (!RwEngineInit(nullptr, 0, 0) || !RwEngineOpen(&params) || !RwEngineStart()) return 1;

    std::vector<RwFrame*> frames(200000);
    const double createDestroy = medianMs([&] {
        for (RwFrame*& f : frames) f = RwFrameCreate();
        for (RwFrame* f : frames) RwFrameDestroy(f);
    });

    std::vector<std::vector<RwFrame*>> trees(1000);
    std::vector<RwObjectHasFrame> objects(1000);
    Rng rng;
    const double build = medianMs([&] {
        for (auto& t : trees) {
            if (!t.empty()) RwFrameDestroyHierarchy(t[0]);
            t.clear();
            for (int i = 0; i < 64; ++i) {
                RwFrame* f = RwFrameCreate();
                if (i > 0) RwFrameAddChild(t[rng.next() % t.size()], f);
                t.push_back(f);
            }
        }
        _rwFrameSyncDirty();
    });
    for (std::size_t i = 0; i < trees.size(); ++i) {
        objects[i].sync = syncNothing;
        _rwObjectHasFrameSetFrame(&objects[i], trees[i][32]);
    }
    const RwV3d step{0.01f, 0.0f, 0.0f};
    const double sync = medianMs([&] {
        for (auto& t : trees) {
            for (RwFrame* f : t) {
                if (f->child == nullptr) RwFrameTranslate(f, &step, rwCOMBINEPOSTCONCAT);
            }
        }
        _rwFrameSyncDirty();
    });
    for (auto& o : objects) _rwObjectHasFrameSetFrame(&o, nullptr);
    for (auto& t : trees) RwFrameDestroyHierarchy(t[0]);

    RwFrame* top = RwFrameCreate();
    RwFrame* bottom = top;
    for (int i = 1; i < 10000; ++i) {
        RwFrame* f = RwFrameCreate();
        RwFrameAddChild(bottom, f);
        bottom = f;
    }
    RwFrameGetLTM(bottom);
    const double deep = medianMs([&] {
        RwFrameTranslate(top, &step, rwCOMBINEPOSTCONCAT);
        RwFrameGetLTM(bottom);
    });
    RwFrameDestroyHierarchy(top);

    std::printf("rwcore_frame_bench (median of 7, ms): create/destroy %.3f  build %.3f  sync %.3f  deep LTM %.3f\n",
                createDestroy, build, sync, deep);
    RwEngineStop(), RwEngineClose(), RwEngineTerm();
    return 0;
}
