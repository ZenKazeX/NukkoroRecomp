// Cross-compiler guard for the native RenderWare math (rwcore/src/matrix.cpp, vector.cpp, frame.cpp, camera.cpp, image.cpp): every floating-point
// function of the core runs over fixed pseudo-random inputs and the bits of every result are hashed. The expected
// digest is the MSVC Release build's, whose results the interpreter oracles prove bit-identical to the game's own
// instructions; any other compiler, flag set or platform must reproduce it exactly - a compiler that fuses a*b+c on
// its own (Clang's and GCC's defaults, without -ffp-contract=off) changes it. Usage: rwcore_fp_digest_tests [--print]
#include "rw/core/camera.h"
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/frame.h"
#include "rw/core/image.h"
#include "rw/core/matrix.h"
#include "rw/core/vector.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace {

constexpr std::uint64_t kExpected = 0xACC0995AB18D8A3Aull;   // the MSVC Release build (see --print; the cameras part 0xD7A9AC3F7B638783)

struct Digest {
    std::uint64_t h = 0xCBF29CE484222325ull;
    void add(const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001B3ull;
    }
    void add(float f) { add(&f, sizeof f); }
    void add(const RwMatrix& m) { add(&m, sizeof m); }
    /// Floats with every NaN as one pattern: which NaN an operation generates is the processor's (x86 and ARM differ).
    void addReals(const float* f, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t bits = std::bit_cast<std::uint32_t>(f[i]);
            const std::uint32_t canonical = (bits & 0x7FFFFFFFu) > 0x7F800000u ? 0x7FC00000u : bits;
            add(&canonical, sizeof canonical);
        }
    }
};

struct Rng {
    std::uint64_t s = 0x2026110220261102ull;
    std::uint32_t next() {
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        return static_cast<std::uint32_t>(s >> 16);
    }
    float real() {   // finite, 2^-20 .. 2^20, both signs
        const std::uint32_t r = next();
        return std::bit_cast<float>(((r & 1u) << 31) | ((107u + next() % 41u) << 23) | (next() & 0x7FFFFFu));
    }
};

RwMatrix matrix(Rng& rng) {
    RwMatrix m{};
    float* f = &m.right.x;
    for (int i : {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14}) f[i] = rng.real();
    m.flags = rng.next() & 0x20003u;
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    RwEngineOpenParams params{nullptr};
    if (!RwEngineInit(nullptr, 0, 0) || !RwEngineOpen(&params) || !RwEngineStart()) {
        std::fprintf(stderr, "rwcore_fp_digest_tests: FAILED: engine start\n");
        return 1;
    }
    Rng rng;
    Digest d;
    for (int i = 0; i < 20000; ++i) {
        const RwMatrix a = matrix(rng), b = matrix(rng);
        RwMatrix m{};
        RwMatrixMultiply(&m, &a, &b), d.add(m);
        MatrixMultiply(&m, &a, &b), d.add(m);
        d.add(_rwMatrixDeterminant(&a)), d.add(_rwMatrixOrthogonalError(&a)), d.add(_rwMatrixNormalError(&a));
        m = a, RwMatrixOptimize(&m, nullptr), d.add(m);
        RwMatrixInvert(&m, &a), d.add(m);
        RwMatrixOrthoNormalize(&m, &a), d.add(m);
        RwMatrixInvert(&m, &m), d.add(m);   // orthonormal path
        const RwV3d v{rng.real(), rng.real(), rng.real()};
        const float angle = rng.real();
        for (RwOpCombineType op : {rwCOMBINEREPLACE, rwCOMBINEPRECONCAT, rwCOMBINEPOSTCONCAT}) {
            m = a, RwMatrixRotate(&m, &v, angle, op), d.add(m);
            m = a, RwMatrixScale(&m, &v, op), d.add(m);
            m = a, RwMatrixTranslate(&m, &v, op), d.add(m);
            m = a, RwMatrixTransform(&m, &b, op), d.add(m);
        }
        m = a, RwMatrixRotateOneMinusCosineSine(&m, &v, rng.real(), rng.real(), rwCOMBINEPOSTCONCAT), d.add(m);
        RwV3d out[3], in[3] = {v, {rng.real(), rng.real(), rng.real()}, {rng.real(), rng.real(), rng.real()}};
        RwV3dTransformPoints(out, in, 3, &a), d.add(out, sizeof out);
        RwV3dTransformPoint(&out[0], &v, &a), RwV3dTransformVector(&out[1], &v, &a), d.add(out, sizeof out);
        d.add(RwV3dNormalize(&out[2], &v)), d.add(&out[2], sizeof out[2]);
        d.add(RwV3dLength(&v));
        const RwV2d w{v.x, v.y};
        d.add(RwV2dLength(&w)), d.add(_rwSqrt(std::bit_cast<float>(rng.next() & 0x7F7FFFFFu)));
        d.add(_rwInvSqrt(std::bit_cast<float>(rng.next() & 0x7F7FFFFFu)));
        RwError e{};
        RwErrorGet(&e);
    }
    const std::uint64_t afterMath = d.h;
    std::uint64_t afterFrames = 0;
    // frames: random hierarchies transformed and synced, every LTM's bits (the frame code's own arithmetic order)
    {
        std::vector<RwFrame*> frames;
        for (int i = 0; i < 400; ++i) {
            RwFrame* f = RwFrameCreate();
            if (i > 0 && rng.next() % 8u != 0) RwFrameAddChild(frames[rng.next() % frames.size()], f);
            frames.push_back(f);
        }
        for (int round = 0; round < 200; ++round) {
            for (int k = 0; k < 20; ++k) {
                RwFrame* f = frames[rng.next() % frames.size()];
                const RwV3d v{rng.real(), rng.real(), rng.real()};
                switch (rng.next() % 3u) {
                    case 0: RwFrameTranslate(f, &v, static_cast<RwOpCombineType>(rng.next() % 3u)); break;
                    case 1: RwFrameRotate(f, &v, rng.real(), static_cast<RwOpCombineType>(rng.next() % 3u)); break;
                    default: RwFrameScale(f, &v, rwCOMBINEPOSTCONCAT); break;
                }
            }
            if (round % 2 == 0) {
                _rwFrameSyncDirty();
            } else {
                for (RwFrame* f : frames) RwFrameGetLTM(f);
            }
            for (RwFrame* f : frames) d.add(f->ltm);
        }
        afterFrames = d.h;
        // cameras on some of those frames (bacamera): perspective and parallel, windows of every shape, offsets, clip
        // ranges, device Z ranges; view matrices, frusta, boxes, Z scale / shift and sphere tests
        for (int round = 0; round < 50; ++round) {
            std::vector<RwCamera*> cameras;
            for (int i = 0; i < 16; ++i) {
                RwCamera* c = RwCameraCreate();
                const RwV2d window{0.1f + static_cast<float>(rng.next() % 1000u) / 300.0f,
                                   0.1f + static_cast<float>(rng.next() % 1000u) / 300.0f};
                const RwV2d offset{static_cast<float>(rng.next() % 200u) / 100.0f - 1.0f,
                                   static_cast<float>(rng.next() % 200u) / 100.0f - 1.0f};
                RwCameraSetProjection(c, rng.next() % 3u == 0 ? rwPARALLEL : rwPERSPECTIVE);
                RwCameraSetViewWindow(c, &window);
                RwCameraSetViewOffset(c, &offset);
                rwSrcGlobals().dOpenDevice.zBufferFar = rng.next() % 2u ? 1.0f : 65535.0f;
                RwCameraSetNearClipPlane(c, 0.01f + static_cast<float>(rng.next() % 1000u) / 100.0f);
                RwCameraSetFarClipPlane(c, 20.0f + static_cast<float>(rng.next() % 100000u));
                RwCameraSetFrame(c, frames[rng.next() % frames.size()]);
                cameras.push_back(c);
            }
            rwSrcGlobals().dOpenDevice.zBufferFar = 1.0f;
            const RwV3d v{rng.real(), rng.real(), rng.real()};
            const float angle = rng.real();   // drawn first, as MSVC evaluates the arguments (right to left)
            RwFrameRotate(frames[rng.next() % frames.size()], &v, angle, rwCOMBINEPRECONCAT);
            _rwFrameSyncDirty();
            for (RwCamera* c : cameras) {
                d.addReals(&c->viewMatrix.right.x, 16);
                d.addReals(&c->viewWindow.x, 11);
                for (const RwFrustumPlane& p : c->frustumPlanes) {
                    d.addReals(&p.plane.normal.x, 4);
                    // a closest-corner byte is its normal's sign bit: meaningless (and the sign of a NaN the
                    // compiler's choice of negated FMA) where the normal is NaN - hashed as 0xFF there
                    const float n[3] = {p.plane.normal.x, p.plane.normal.y, p.plane.normal.z};
                    const RwUInt8 b[3] = {p.closestX, p.closestY, p.closestZ};
                    for (int k = 0; k < 3; ++k) {
                        const RwUInt8 v = n[k] != n[k] ? RwUInt8{0xFF} : b[k];
                        d.add(&v, 1);
                    }
                }
                d.addReals(&c->frustumBoundBox.sup.x, 6);
                d.addReals(&c->frustumCorners[0].x, 24);
                for (int k = 0; k < 8; ++k) {
                    const RwV3d& a = c->frustumCorners[rng.next() % 8u];
                    const RwSphere s{{a.x + rng.real(), a.y + rng.real(), a.z + rng.real()}, rng.real()};
                    const RwFrustumTestResult r = RwCameraFrustumTestSphere(c, &s);
                    d.add(&r, sizeof r);
                }
                RwCameraDestroy(c);
            }
        }
        std::vector<RwFrame*> roots;   // first: destroying a hierarchy frees frames later in the list
        for (RwFrame* f : frames) {
            if (f->object.parent == nullptr) roots.push_back(f);
        }
        for (RwFrame* r : roots) RwFrameDestroyHierarchy(r);
    }
    // the image module's gamma tables (single-precision steps around MSL's pow; rwcore/src/image.cpp) for 400 gammas
    const std::uint64_t afterCameras = d.h;
    for (int n = 0; n < 400; ++n) {
        const float gamma = n < 200 ? 0.05f + 0.025f * static_cast<float>(n) : std::fabs(rng.real());
        RwImageSetGamma(gamma);
        const rwImageGlobals& g = rwModuleGlobals<rwImageGlobals>(imageModule);
        d.add(g.gammaTable, sizeof g.gammaTable);
        d.add(g.invGammaTable, sizeof g.invGammaTable);
    }
    RwEngineStop(), RwEngineClose(), RwEngineTerm();
    if (argc > 1 && std::strcmp(argv[1], "--print") == 0) {
        std::printf("math 0x%016llX, frames 0x%016llX, cameras 0x%016llX, all ", static_cast<unsigned long long>(afterMath),
                    static_cast<unsigned long long>(afterFrames), static_cast<unsigned long long>(afterCameras));
        std::printf("0x%016llXull\n", static_cast<unsigned long long>(d.h));
        return 0;
    }
    if (d.h != kExpected) {
        std::fprintf(stderr, "rwcore_fp_digest_tests: FAILED: digest 0x%016llX, expected 0x%016llX\n",
                     static_cast<unsigned long long>(d.h), static_cast<unsigned long long>(kExpected));
        return 1;
    }
    std::printf("rwcore_fp_digest_tests: digest 0x%016llX matches the oracle-proven build\n",
                static_cast<unsigned long long>(d.h));
    return 0;
}
