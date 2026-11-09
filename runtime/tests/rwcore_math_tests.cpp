// Independent checks of the native RenderWare math (rwcore/src/matrix.cpp, vector.cpp): the oracles prove the port
// equals the original instruction for instruction; this test checks that both are RIGHT, against double-precision
// references and mathematical properties, with no interpreter involved - a shared misunderstanding (of the original or
// of the interpreter's floating point) would pass the oracles but not this. Also: every function that allows dst ==
// src gives exactly the same bits in place as separate; the square-root tables meet their 11-bit design precision.
// (Whether a compiler contracts a*b+c on its own is checked by running the interpreter oracles under Clang.) The engine is brought up through RwEngineInit / Open / Start.
//
// Usage: rwcore_math_tests
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/matrix.h"
#include "rw/core/vector.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Rng {
    std::uint64_t s = 0x9E3779B97F4A7C15ull;
    std::uint32_t next() {
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        return static_cast<std::uint32_t>(s >> 16);
    }
    float uniform(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(next() & 0xFFFFFF) / 16777216.0f; }
};

struct D3 {
    double x, y, z;
};
struct DM {   // a double matrix in RenderWare's row-vector convention: p' = p * M
    D3 r, u, a, p;
};
D3 d(const RwV3d& v) { return {v.x, v.y, v.z}; }
DM d(const RwMatrix& m) { return {d(m.right), d(m.up), d(m.at), d(m.pos)}; }
D3 rowTimes(const D3& v, const DM& m, double w) {
    return {v.x * m.r.x + v.y * m.u.x + v.z * m.a.x + w * m.p.x, v.x * m.r.y + v.y * m.u.y + v.z * m.a.y + w * m.p.y,
            v.x * m.r.z + v.y * m.u.z + v.z * m.a.z + w * m.p.z};
}
DM mul(const DM& a, const DM& b) { return {rowTimes(a.r, b, 0), rowTimes(a.u, b, 0), rowTimes(a.a, b, 0), rowTimes(a.p, b, 1)}; }
double dot(const D3& p, const D3& q) { return p.x * q.x + p.y * q.y + p.z * q.z; }
double det3(const DM& m) {
    return m.r.x * (m.u.y * m.a.z - m.u.z * m.a.y) - m.r.y * (m.u.x * m.a.z - m.u.z * m.a.x) +
           m.r.z * (m.u.x * m.a.y - m.u.y * m.a.x);
}
double maxDiff(const DM& a, const DM& b) {
    const double* pa = &a.r.x;
    const double* pb = &b.r.x;
    double m = 0;
    for (int i = 0; i < 12; ++i) m = std::fmax(m, std::fabs(pa[i] - pb[i]));
    return m;
}
double scale(const DM& a) {
    const double* p = &a.r.x;
    double m = 1;
    for (int i = 0; i < 12; ++i) m = std::fmax(m, std::fabs(p[i]));
    return m;
}

RwMatrix randomMatrix(Rng& rng, float range) {
    RwMatrix m{};
    float* f = &m.right.x;
    for (int i : {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14}) f[i] = rng.uniform(-range, range);
    m.flags = 0;
    return m;
}

/// A rotation (orthonormal, det +1) built in double from a random axis and angle.
RwMatrix randomRotation(Rng& rng) {
    D3 k{rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)};
    const double n = std::sqrt(dot(k, k)) + 1e-9;
    k = {k.x / n, k.y / n, k.z / n};
    const double t = rng.uniform(-3.14f, 3.14f), c = std::cos(t), s = std::sin(t), C = 1 - c;
    RwMatrix m{};
    m.right = {static_cast<float>(c + k.x * k.x * C), static_cast<float>(k.x * k.y * C + k.z * s),
               static_cast<float>(k.x * k.z * C - k.y * s)};
    m.up = {static_cast<float>(k.y * k.x * C - k.z * s), static_cast<float>(c + k.y * k.y * C),
            static_cast<float>(k.y * k.z * C + k.x * s)};
    m.at = {static_cast<float>(k.z * k.x * C + k.y * s), static_cast<float>(k.z * k.y * C - k.x * s),
            static_cast<float>(c + k.z * k.z * C)};
    m.pos = {rng.uniform(-100, 100), rng.uniform(-100, 100), rng.uniform(-100, 100)};
    m.flags = rwMATRIXTYPEORTHONORMAL;
    return m;
}

bool sameBits(const RwMatrix& a, const RwMatrix& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

}  // namespace

int main() {
    try {
        RwEngineOpenParams params{nullptr};
        require(RwEngineInit(nullptr, 0, 0) && RwEngineOpen(&params) && RwEngineStart(), "engine start");
        Rng rng;
        unsigned checks = 0;
        const double eps = std::ldexp(1.0, -23);

        // ---- square-root tables: 11-bit design precision over every exponent --------------------------------------
        {
            double worstSqrt = 0, worstInv = 0;
            for (std::uint32_t bits = 0x00800000u; bits < 0x7F000000u; bits += 0x1F3u) {
                const float x = std::bit_cast<float>(bits);
                const double s = std::sqrt(static_cast<double>(x));
                worstSqrt = std::fmax(worstSqrt, std::fabs(_rwSqrt(x) - s) / s);
                worstInv = std::fmax(worstInv, std::fabs(_rwInvSqrt(x) * s - 1.0));
            }
            require(_rwSqrt(0.0f) == 0.0f, "_rwSqrt(0)");
            std::printf("  _rwSqrt max relative error %.3g, _rwInvSqrt %.3g (design: 2^-12 = %.3g)\n", worstSqrt, worstInv,
                        std::ldexp(1.0, -12));
            require(worstSqrt <= std::ldexp(1.0, -12) && worstInv <= std::ldexp(1.0, -12), "table precision");
            checks += 2;
        }

        for (unsigned c = 0; c < 20000; ++c) {
            const RwMatrix A = randomMatrix(rng, 4), B = randomMatrix(rng, 4), R = randomRotation(rng);

            // multiply: against double, and every overlap gives the same bits
            RwMatrix P{};
            RwMatrixMultiply(&P, &A, &B);
            const DM ref = mul(d(A), d(B));
            require(maxDiff(d(P), ref) <= 8 * eps * scale(ref) * 4, "RwMatrixMultiply vs double");
            RwMatrix inA = A, inB = B;
            RwMatrixMultiply(&inA, &inA, &B);
            RwMatrixMultiply(&inB, &A, &inB);
            P.pad1 = inA.pad1, P.pad2 = inA.pad2, P.pad3 = inA.pad3;
            require(sameBits(inA, P), "RwMatrixMultiply with dst == a");
            P.pad1 = inB.pad1, P.pad2 = inB.pad2, P.pad3 = inB.pad3;
            require(sameBits(inB, P), "RwMatrixMultiply with dst == b");
            RwMatrix S{};
            MatrixMultiply(&S, &A, &B);
            require(maxDiff(d(S), ref) <= 32 * eps * scale(ref), "MatrixMultiply (scalar) vs double");

            // determinant
            const double det = det3(d(A));
            require(std::fabs(_rwMatrixDeterminant(&A) - det) <= 64 * eps * std::fmax(1.0, std::pow(scale(d(A)), 3)),
                    "_rwMatrixDeterminant vs double");

            // inverse: inverse * matrix = identity (well-conditioned general matrices; rotations by the fast path)
            if (std::fabs(det) > 1.0) {
                RwMatrix I{}, back{};
                RwMatrixInvert(&I, &A);
                RwMatrixMultiply(&back, &I, &A);
                const DM e = mul(d(I), d(A));
                const DM id{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}};
                require(maxDiff(e, id) <= 1e-3, "RwMatrixInvert(A) * A = I");
                RwMatrix inPlace = A;
                RwMatrixInvert(&inPlace, &inPlace);
                require(sameBits(inPlace, I), "RwMatrixInvert in place");
            }
            {
                RwMatrix I{};
                RwMatrixInvert(&I, &R);
                const DM e = mul(d(I), d(R));
                const DM id{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}};
                require(maxDiff(e, id) <= 1e-4, "RwMatrixInvert(rotation) * rotation = I");
                RwMatrix inPlace = R;
                RwMatrixInvert(&inPlace, &inPlace);
                require(sameBits(inPlace, I), "RwMatrixInvert(rotation) in place");
            }

            // ortho-normalize: orthonormal, right-handed, close to a rotation it was made from
            {
                RwMatrix noisy = R;
                float* f = &noisy.right.x;
                for (int i : {0, 1, 2, 4, 5, 6, 8, 9, 10}) f[i] *= rng.uniform(0.9f, 1.1f);
                RwMatrix O{};
                RwMatrixOrthoNormalize(&O, &noisy);
                const DM o = d(O);
                require(std::fabs(dot(o.r, o.r) - 1) < 2e-3 && std::fabs(dot(o.u, o.u) - 1) < 2e-3 &&
                            std::fabs(dot(o.a, o.a) - 1) < 2e-3 && std::fabs(dot(o.r, o.u)) < 2e-3 &&
                            std::fabs(dot(o.u, o.a)) < 2e-3 && std::fabs(dot(o.a, o.r)) < 2e-3 &&
                            std::fabs(det3(o) - 1) < 5e-3 && maxDiff(o, d(R)) < 0.2,
                        "RwMatrixOrthoNormalize gives an orthonormal right-handed matrix");
                RwMatrix inPlace = noisy;
                RwMatrixOrthoNormalize(&inPlace, &inPlace);
                require(sameBits(inPlace, O), "RwMatrixOrthoNormalize in place");
            }

            // rotate: Rodrigues, and the combine orders (replace / pre: R * M / post: M * R)
            {
                RwV3d axis{rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)};
                if (std::fabs(axis.x) + std::fabs(axis.y) + std::fabs(axis.z) < 0.1f) axis.x = 1;
                const float degrees = rng.uniform(-360, 360);
                RwMatrix rot = A;
                RwMatrixRotate(&rot, &axis, degrees, rwCOMBINEREPLACE);
                const double n = std::sqrt(dot(d(axis), d(axis)));
                const D3 k{axis.x / n, axis.y / n, axis.z / n};
                const double t = degrees * 3.14159265358979323846 / 180.0;
                const D3 v{1.0, -2.0, 0.5};
                const double kv = dot(k, v);
                const D3 kxv{k.y * v.z - k.z * v.y, k.z * v.x - k.x * v.z, k.x * v.y - k.y * v.x};
                const D3 expect{v.x * std::cos(t) + kxv.x * std::sin(t) + k.x * kv * (1 - std::cos(t)),
                                v.y * std::cos(t) + kxv.y * std::sin(t) + k.y * kv * (1 - std::cos(t)),
                                v.z * std::cos(t) + kxv.z * std::sin(t) + k.z * kv * (1 - std::cos(t))};
                const D3 got = rowTimes(v, d(rot), 0);
                // the axis is normalized with _rwInvSqrt (the 11-bit table, as in the original): the rotation is
                // as exact as that, |v| * 2^-12 per normalization, a few times over
                const double bound = 4 * std::ldexp(1.0, -12) * std::sqrt(dot(v, v));
                require(std::fabs(got.x - expect.x) < bound && std::fabs(got.y - expect.y) < bound &&
                            std::fabs(got.z - expect.z) < bound,
                        "RwMatrixRotate is a right-hand rotation about the axis (Rodrigues)");
                RwMatrix pre = A, post = A;
                RwMatrixRotate(&pre, &axis, degrees, rwCOMBINEPRECONCAT);
                RwMatrixRotate(&post, &axis, degrees, rwCOMBINEPOSTCONCAT);
                require(maxDiff(d(pre), mul(d(rot), d(A))) < 1e-4 * scale(d(A)) &&
                            maxDiff(d(post), mul(d(A), d(rot))) < 1e-4 * scale(d(A)),
                        "RwMatrixRotate pre = R * M, post = M * R");
            }

            // scale / translate / transform combine orders
            {
                const RwV3d s{rng.uniform(-3, 3), rng.uniform(-3, 3), rng.uniform(-3, 3)};
                const RwV3d t{rng.uniform(-50, 50), rng.uniform(-50, 50), rng.uniform(-50, 50)};
                RwMatrix sc{}, tr{};
                RwMatrixScale(&sc, &s, rwCOMBINEREPLACE);
                RwMatrixTranslate(&tr, &t, rwCOMBINEREPLACE);
                require(sc.right.x == s.x && sc.up.y == s.y && sc.at.z == s.z && sc.pos.x == 0 && tr.pos.y == t.y &&
                            tr.right.x == 1 && tr.up.y == 1,
                        "RwMatrixScale / Translate replace");
                for (RwOpCombineType op : {rwCOMBINEPRECONCAT, rwCOMBINEPOSTCONCAT}) {
                    RwMatrix ms = A, mt = A, mx = A;
                    RwMatrixScale(&ms, &s, op);
                    RwMatrixTranslate(&mt, &t, op);
                    RwMatrixTransform(&mx, &B, op);
                    const bool pre = op == rwCOMBINEPRECONCAT;
                    require(maxDiff(d(ms), pre ? mul(d(sc), d(A)) : mul(d(A), d(sc))) < 1e-4 * scale(d(A)) * 3 &&
                                maxDiff(d(mt), pre ? mul(d(tr), d(A)) : mul(d(A), d(tr))) < 1e-4 * scale(d(A)) * 60 &&
                                maxDiff(d(mx), pre ? mul(d(B), d(A)) : mul(d(A), d(B))) < 1e-4 * scale(d(A)) * 4,
                            "RwMatrixScale / Translate / Transform combine orders");
                }
            }

            // transforms of points and vectors, separate and in place
            {
                RwV3d pts[8], out[8], inPlace[8];
                for (RwV3d& p : pts) p = {rng.uniform(-10, 10), rng.uniform(-10, 10), rng.uniform(-10, 10)};
                RwV3dTransformPoints(out, pts, 8, &A);
                std::memcpy(inPlace, pts, sizeof pts);
                RwV3dTransformPoints(inPlace, inPlace, 8, &A);
                require(std::memcmp(out, inPlace, sizeof out) == 0, "RwV3dTransformPoints in place");
                for (int i = 0; i < 8; ++i) {
                    const D3 e = rowTimes(d(pts[i]), d(A), 1);
                    require(std::fabs(out[i].x - e.x) + std::fabs(out[i].y - e.y) + std::fabs(out[i].z - e.z) <= 1e-4 * 40,
                            "RwV3dTransformPoints vs double");
                    RwV3d one{}, vec{};
                    RwV3dTransformPoint(&one, &pts[i], &A);
                    RwV3dTransformVector(&vec, &pts[i], &A);
                    const D3 ev = rowTimes(d(pts[i]), d(A), 0);
                    require(std::memcmp(&one, &out[i], sizeof one) == 0 &&
                                std::fabs(vec.x - ev.x) + std::fabs(vec.y - ev.y) + std::fabs(vec.z - ev.z) <= 1e-4 * 40,
                            "RwV3dTransformPoint / Vector");
                }
            }

            // lengths and normalize
            {
                const RwV3d v{rng.uniform(-100, 100), rng.uniform(-100, 100), rng.uniform(-100, 100)};
                const double len = std::sqrt(dot(d(v), d(v)));
                RwV3d n{}, inPlace = v;
                const RwReal l = RwV3dNormalize(&n, &v);
                RwV3dNormalize(&inPlace, &inPlace);
                require(std::fabs(l - len) <= len * std::ldexp(1.0, -11) && std::fabs(RwV3dLength(&v) - len) <= len * std::ldexp(1.0, -11) &&
                            std::fabs(std::sqrt(dot(d(n), d(n))) - 1) <= std::ldexp(1.0, -10) &&
                            std::memcmp(&n, &inPlace, sizeof n) == 0,
                        "RwV3dNormalize / RwV3dLength");
                const RwV2d w{v.x, v.y};
                require(std::fabs(RwV2dLength(&w) - std::hypot(double{v.x}, double{v.y})) <= len * std::ldexp(1.0, -11),
                        "RwV2dLength");
            }
            checks += 14;
        }

        // a zero vector: the error, a length of 0
        {
            const RwV3d zero{0, 0, 0};
            RwV3d n{};
            RwError e{};
            RwErrorGet(&e);
            require(RwV3dNormalize(&n, &zero) == 0.0f && RwErrorGet(&e)->errorCode == 0x19, "RwV3dNormalize(0): E_RW_ZEROLENGTH");
            ++checks;
        }

        require(RwEngineStop() && RwEngineClose() && RwEngineTerm(), "engine shutdown");
        std::printf("rwcore_math_tests: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_math_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
