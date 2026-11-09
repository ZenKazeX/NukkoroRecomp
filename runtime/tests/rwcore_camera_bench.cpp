// Benchmark of the native RenderWare cameras (rwcore/src/camera.cpp). Not a test (no pass / fail): median of 7 runs per
// workload, for comparing builds of the camera code.
//   sync:    10,000 cameras (perspective and parallel, 16:9 .. 32:10 windows) on frames under 100 moving roots; the
//            roots rotated, then one _rwFrameSyncDirty, 10 rounds: 100,000 frame LTMs + view matrices, frusta and boxes
//            (ns per camera, its frame included)
//   spheres: 1,000,000 RwCameraFrustumTestSphere calls: points between corners, spheres there, spheres far off (the
//            results are printed; ns per test)
//   setters: 1,000,000 near / far plane changes (the Z scale / shift and the frame mark; ns per call)
//
// Usage: rwcore_camera_bench
#include "rw/core/camera.h"
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
    float real(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(next() & 0xFFFFFF) / 16777216.0f; }
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

}  // namespace

int main() {
    RwEngineOpenParams params{nullptr};
    if (!RwEngineInit(nullptr, 0, 0) || !RwEngineOpen(&params) || !RwEngineStart()) return 1;
    Rng rng;
    const float aspects[] = {16.0f / 9.0f, 16.0f / 10.0f, 21.0f / 9.0f, 32.0f / 10.0f};

    std::vector<RwFrame*> roots(100);
    std::vector<RwFrame*> frames(10000);
    std::vector<RwCamera*> cameras(10000);
    for (RwFrame*& r : roots) r = RwFrameCreate();
    for (std::size_t i = 0; i < cameras.size(); ++i) {
        frames[i] = RwFrameCreate();
        RwFrameAddChild(roots[i % roots.size()], frames[i]);
        const RwV3d move{rng.real(-50, 50), rng.real(-50, 50), rng.real(-50, 50)};
        RwFrameTranslate(frames[i], &move, rwCOMBINEREPLACE);
        cameras[i] = RwCameraCreate();
        if (i % 4 == 3) RwCameraSetProjection(cameras[i], rwPARALLEL);
        const RwV2d window{0.4142f * aspects[i % 4], 0.4142f};
        RwCameraSetViewWindow(cameras[i], &window);
        RwCameraSetNearClipPlane(cameras[i], 0.1f);
        RwCameraSetFarClipPlane(cameras[i], 5000.0f);
        RwCameraSetFrame(cameras[i], frames[i]);
    }
    _rwFrameSyncDirty();
    const RwV3d axis{0.3f, 0.9f, 0.1f};
    const double sync = medianMs([&] {
        for (int round = 0; round < 10; ++round) {
            for (RwFrame* r : roots) RwFrameRotate(r, &axis, 1.5f, rwCOMBINEPRECONCAT);
            _rwFrameSyncDirty();
        }
    });

    RwCamera* c = cameras[0];
    std::vector<RwSphere> spheres(1000000);
    for (std::size_t i = 0; i < spheres.size(); ++i) {
        const int kind = static_cast<int>(i % 3);
        const RwV3d& a = c->frustumCorners[rng.next() % 8], &b = c->frustumCorners[rng.next() % 8];
        const float t = rng.real(0.1f, 0.9f);
        RwV3d p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
        if (kind == 2) p = {p.x + rng.real(-20000, 20000), p.y + rng.real(-20000, 20000), p.z + rng.real(-20000, 20000)};
        spheres[i] = {p, kind == 0 ? 0.0f : rng.real(1, 50)};
    }
    unsigned counts[3] = {};
    const double sphereMs = medianMs([&] {
        counts[0] = counts[1] = counts[2] = 0;
        for (const RwSphere& s : spheres) ++counts[RwCameraFrustumTestSphere(c, &s)];
    });

    float sink = 0;
    const double setters = medianMs([&] {
        for (int i = 0; i < 500000; ++i) {
            RwCameraSetNearClipPlane(c, 0.1f + static_cast<float>(i & 7) * 0.01f);
            RwCameraSetFarClipPlane(c, 4000.0f + static_cast<float>(i & 15));
            sink += c->zShift;
        }
    });

    std::printf("rwcore_camera_bench (median of 7, ms):\n");
    std::printf("  sync     %8.2f  (%.1f ns per camera, its frame's LTM sync included)\n", sync, sync * 1e6 / (10.0 * cameras.size()));
    std::printf("  spheres  %8.2f  (%.2f ns per test; outside %u, boundary %u, inside %u)\n", sphereMs,
                sphereMs * 1e6 / static_cast<double>(spheres.size()), counts[0], counts[1], counts[2]);
    std::printf("  setters  %8.2f  (%.2f ns per call; %g)\n", setters, setters * 1e6 / 1e6, static_cast<double>(sink));
    for (RwCamera* cam : cameras) RwCameraDestroy(cam);
    for (RwFrame* r : roots) RwFrameDestroyHierarchy(r);
    RwEngineStop(), RwEngineClose(), RwEngineTerm();
    return 0;
}
