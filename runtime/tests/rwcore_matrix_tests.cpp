// Oracle for the native RenderWare core matrices (rwcore/src/matrix.cpp): the ORIGINAL functions of PAL main.dol run
// in the reference interpreter in a guest RenderWare environment (rwcore_oracle.h), the native functions on a native
// 64-bit engine instance with the vector and matrix modules, and every output bit is compared.
// liveness-roots: reference-interpreter-only
//
// Input domain (stated, not implied): finite floats from 2^-20 to 2^20 in magnitude (2^-6 to 2^6 for the inverse,
// whose products must not overflow), both signs, plus +0 and -0, random 32-bit flags and pad words, angles from
// -720 to 720 degrees. NaN payloads and subnormals are outside it. Pad words are compared except where the original
// copies a stack temporary's never-written pads (the concatenating combine paths of RwMatrixRotateOneMinusCosineSine,
// RwMatrixRotate and RwMatrixTransform, and RwMatrixRotateOneMinusCosineSine's replace path): there the native
// writes 0 and the oracle compares the rows and flags. RwMatrixCreate's pads are the allocator's.
//
// Usage: rwcore_matrix_tests <main.dol>
#include "rw/core/error.h"
#include "rw/core/matrix.h"
#include "rw/core/vector.h"
#include "rwcore_oracle.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace {
using namespace rworacle;

constexpr Original kMatrixMultiplyScalar{"MatrixMultiply", 0x8047F150u, 0x198u, 0x9421FF90u, 0x4E800020u};
constexpr Original kDeterminant{"_rwMatrixDeterminant", 0x8047F754u, 0x4Cu, 0xC1230010u, 0x4E800020u};
constexpr Original kOrthogonalError{"_rwMatrixOrthogonalError", 0x8047F7A0u, 0x58u, 0xC0030024u, 0x4E800020u};
constexpr Original kNormalError{"_rwMatrixNormalError", 0x8047F7F8u, 0x80u, 0xC0230010u, 0x4E800020u};
constexpr Original kSetTolerances{"RwEngineSetMatrixTolerances", 0x8047F9B0u, 0x30u, 0x808D6108u, 0x4E800020u};
constexpr Original kOptimize{"RwMatrixOptimize", 0x8047F9E0u, 0x210u, 0x9421FFC0u, 0x4E800020u};
constexpr Original kUpdate{"RwMatrixUpdate", 0x8047FBF0u, 24u, 0x3C80FFFEu, 0x4E800020u};
constexpr Original kMultiply{"RwMatrixMultiply", 0x8047FC08u, 252u, 0x9421FFD0u, 0x4E800020u};
constexpr Original kOrthoNormalize{"RwMatrixOrthoNormalize", 0x8047FD04u, 0x20u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRotateOmcs{"RwMatrixRotateOneMinusCosineSine", 0x8047FD24u, 0x2F4u, 0x9421FF50u, 0x4E800020u};
constexpr Original kRotate{"RwMatrixRotate", 0x80480018u, 0xE8u, 0x9421FFB0u, 0x4E800020u};
constexpr Original kInvert{"RwMatrixInvert", 0x80480100u, 0x2E8u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kScale{"RwMatrixScale", 0x804803E8u, 0x204u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kTranslate{"RwMatrixTranslate", 0x804805ECu, 0x190u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kTransform{"RwMatrixTransform", 0x8048077Cu, 0x22Cu, 0x9421FF60u, 0x4E800020u};
constexpr Original kDestroy{"RwMatrixDestroy", 0x804809A8u, 0x3Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kCreate{"RwMatrixCreate", 0x804809E4u, 0x94u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kTransformPoint{"RwV3dTransformPoint", 0x804834A8u, 76u, 0xE1040000u, 0x4E800020u};
constexpr Original kVectorOpen{"_rwVectorOpen", 0x8048363Cu, 0x398u, 0x9421FFB0u, 0x4E800020u};
constexpr Original kErrorOpen{"_rwErrorOpen", 0x8047EB88u, 0x38u, 0x390D6100u, 0x4E800020u};

constexpr u32 kGuestErrorOffset = 0x200u, kGuestVectorOffset = 0x210u, kGuestMatrixOffset = 0x240u;
constexpr u32 kGuestFreeList = 0x81300000u;   // the matrix free list the stubs are handed (never dereferenced)
constexpr u32 kA = kBuffer, kB = kBuffer + 0x40u, kD = kBuffer + 0x80u, kV = kBuffer + 0xC0u, kT = kBuffer + 0xE0u;

// ---- the native engine -----------------------------------------------------------------------------------------
std::vector<std::pair<RwFreeList*, RwUInt32>> g_allocs;
std::vector<std::pair<RwFreeList*, void*>> g_frees;
alignas(16) unsigned char g_alloc_space[64];
RwFreeList* const kNativeFreeList = reinterpret_cast<RwFreeList*>(&g_allocs);   // an identity, never dereferenced

void* nativeMemoryAlloc(RwFreeList* fl, RwUInt32 hint) {
    g_allocs.push_back({fl, hint});
    std::memset(g_alloc_space, 0, sizeof g_alloc_space);
    return g_alloc_space;
}
RwFreeList* nativeMemoryFree(RwFreeList* fl, void* data) {
    g_frees.push_back({fl, data});
    return fl;
}

struct NativeEngine {
    static constexpr RwInt32 kErrorOffset = static_cast<RwInt32>((sizeof(RwGlobals) + 15) & ~std::size_t{15});
    static constexpr RwInt32 kVectorOffset = kErrorOffset + 16;
    static constexpr RwInt32 kMatrixOffset = kVectorOffset + 32;
    std::vector<std::max_align_t> block = std::vector<std::max_align_t>(64 + sizeof(RwGlobals) / sizeof(std::max_align_t));

    NativeEngine() {
        RwEngineInstance = block.data();
        rwSrcGlobals() = RwGlobals{};
        rwSrcGlobals().memoryFuncs = rwHostMemoryFunctions();
        rwSrcGlobals().memoryAlloc = nativeMemoryAlloc;
        rwSrcGlobals().memoryFree = nativeMemoryFree;
        errorModule = {};
        vectorModule = {};
        matrixModule = {kMatrixOffset, 1};
        _rwErrorOpen(RwEngineInstance, kErrorOffset, 8);
        require(_rwVectorOpen(RwEngineInstance, kVectorOffset, 8) == RwEngineInstance, "native vector module");
        rwMatrixGlobals& m = rwModuleGlobals<rwMatrixGlobals>(matrixModule);
        m.matrixFreeList = kNativeFreeList;
        m.matrixOptimizations = rwMATRIXOPTIMIZE_IDENTITY;
        m.multMatrix = MatrixMultiply;
        m.tolerance = {0.01f, 0.01f, 0.01f};
    }
};

void guest_environment(Guest& g) {
    g.reset_engine();
    for (u32 a : {kErrorModule, kErrorModule + 4u, kVectorModule, kVectorModule + 4u}) g.memory().write32(a, 0);
    g.call(kErrorOpen.entry, {kEngine, kGuestErrorOffset, 8});
    g.call(kVectorOpen.entry, {kEngine, kGuestVectorOffset, 8});
    require(g.r3() == kEngine, "the original _rwVectorOpen failed");
    // the matrix module as _rwMatrixOpen leaves it (its free list is the stubs' identity)
    g.memory().write32(kMatrixModule, kGuestMatrixOffset);
    g.memory().write32(kMatrixModule + 4, 1);
    g.memory().write32(kEngine + kGuestMatrixOffset + 0x0, kGuestFreeList);
    g.memory().write32(kEngine + kGuestMatrixOffset + 0x4, rwMATRIXOPTIMIZE_IDENTITY);
    g.memory().write32(kEngine + kGuestMatrixOffset + 0x8, kMatrixMultiplyScalar.entry);
    for (u32 i = 0; i < 3; ++i) g.write_float(kEngine + kGuestMatrixOffset + 0xC + 4 * i, 0.01f);
}

u32 bits(float v) { return std::bit_cast<u32>(v); }

void put(Guest& g, u32 a, const RwMatrix& m) {
    std::array<u32, 16> words{};
    std::memcpy(words.data(), &m, sizeof m);
    for (u32 i = 0; i < 16; ++i) g.memory().write32(a + 4 * i, words[i]);
}

void put(Guest& g, u32 a, const RwV3d& v) {
    g.write_float(a, v.x);
    g.write_float(a + 4, v.y);
    g.write_float(a + 8, v.z);
}

/// The guest matrix at a equals m: all 16 words, or (pads = false) the rows and flags.
void same(Guest& g, u32 a, const RwMatrix& m, bool pads, const char* what) {
    std::array<u32, 16> words{};
    std::memcpy(words.data(), &m, sizeof m);
    for (u32 i = 0; i < 16; ++i) {
        if (!pads && (i == 7 || i == 11 || i == 15)) continue;
        if (g.memory().peek32(a + 4 * i) != words[i]) {
            std::fprintf(stderr, "word %u: original %08X native %08X\n", i, g.memory().peek32(a + 4 * i), words[i]);
            require(false, what);
        }
    }
}

void same_error(Guest& g) {
    const RwError& held = rwModuleGlobals<RwError>(errorModule);
    require(g.memory().peek32(kEngine + kGuestErrorOffset) == static_cast<u32>(held.pluginID) &&
                g.memory().peek32(kEngine + kGuestErrorOffset + 4) == static_cast<u32>(held.errorCode),
            "the held error differs from the original's");
}

void clear_error(Guest& g) {
    RwError out{};
    RwErrorGet(&out);
    g.memory().write32(kEngine + kGuestErrorOffset, 0);
    g.memory().write32(kEngine + kGuestErrorOffset + 4, static_cast<u32>(rwERRORNONE));
}

RwMatrix random_matrix(Rng& rng, int lo = -20, int hi = 20) {
    RwMatrix m{};
    for (RwV3d* row : {&m.right, &m.up, &m.at, &m.pos}) *row = {rng.real(lo, hi), rng.real(lo, hi), rng.real(lo, hi)};
    m.flags = rng.next();
    m.pad1 = rng.next();
    m.pad2 = rng.next();
    m.pad3 = rng.next();
    return m;
}

/// A matrix near orthonormal (a rotation by the native RwMatrixRotateOneMinusCosineSine, then small noise), so
/// RwMatrixOptimize, RwMatrixOrthoNormalize and RwMatrixInvert see both sides of their tests.
RwMatrix near_rotation(Rng& rng) {
    RwMatrix m = random_matrix(rng);
    RwV3d axis{rng.real(-2, 2), rng.real(-2, 2), rng.real(-2, 2)};
    if (axis.x == 0 && axis.y == 0 && axis.z == 0) axis.x = 1;
    const float inv = 1.0f / std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    axis = {axis.x * inv, axis.y * inv, axis.z * inv};
    const float angle = static_cast<float>(rng.next() % 6283u) / 1000.0f;
    RwMatrixRotateOneMinusCosineSine(&m, &axis, 1.0f - std::cos(angle), std::sin(angle), rwCOMBINEREPLACE);
    const float noise = (rng.next() & 1u) ? 0.0f : std::ldexp(1.0f, -static_cast<int>(rng.next() % 20u));
    for (RwV3d* row : {&m.right, &m.up, &m.at}) {
        row->x += noise * (static_cast<float>(rng.next() % 3u) - 1.0f);
        row->y += noise * (static_cast<float>(rng.next() % 3u) - 1.0f);
    }
    if (rng.next() % 4u == 0) m.pos = {0, 0, 0};
    m.flags = rng.next();
    m.pad1 = rng.next();
    m.pad2 = rng.next();
    m.pad3 = rng.next();
    return m;
}

RwOpCombineType combine_op(Rng& rng) {
    const u32 r = rng.next() % 16u;
    if (r == 15) return static_cast<RwOpCombineType>(3);           // invalid
    if (r == 14) return static_cast<RwOpCombineType>(-1);          // invalid
    return static_cast<RwOpCombineType>(r % 3u);
}

bool valid_op(RwOpCombineType op) { return op >= 0 && op <= 2; }

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rwcore_matrix_tests main.dol");
        Guest g(argv[1]);
        for (const Original& f : {kMatrixMultiplyScalar, kDeterminant, kOrthogonalError, kNormalError, kSetTolerances,
                                  kOptimize, kUpdate, kMultiply, kOrthoNormalize, kRotateOmcs, kRotate, kInvert,
                                  kScale, kTranslate, kTransform, kDestroy, kCreate, kTransformPoint, kVectorOpen,
                                  kErrorOpen}) {
            g.check_pinned(f);
        }
        u32 alloc_at = 0;
        std::vector<std::pair<u32, u32>> guest_allocs, guest_frees;
        g.stub(kMemoryAllocStub, [&](Cpu& c) {
            guest_allocs.push_back({c.gpr[3], c.gpr[4]});
            alloc_at = g.heap_alloc(64);
            for (u32 i = 0; i < 64; i += 4) g.memory().write32(alloc_at + i, 0);
            c.gpr[3] = alloc_at;
        });
        g.stub(kMemoryFreeStub, [&](Cpu& c) { guest_frees.push_back({c.gpr[3], c.gpr[4]}); });

        NativeEngine native;
        guest_environment(g);
        rwMatrixGlobals& globals = rwModuleGlobals<rwMatrixGlobals>(matrixModule);
        Rng rng;
        u64 instructions = 0;
        unsigned checks = 0;
        constexpr unsigned kCases = 20000;

        for (unsigned c = 0; c < kCases; ++c) {
            // ---- RwMatrixMultiply (paired single) and MatrixMultiply (scalar; dst may be a or b) ---------------
            {
                const RwMatrix a = random_matrix(rng), b = random_matrix(rng);
                RwMatrix dst = random_matrix(rng);
                put(g, kA, a);
                put(g, kB, b);
                put(g, kD, dst);
                instructions += g.call(kMultiply.entry, {kD, kA, kB});
                require(g.r3() == kD && RwMatrixMultiply(&dst, &a, &b) == &dst, "RwMatrixMultiply must return dst");
                same(g, kD, dst, true, "RwMatrixMultiply differs from the original");

                const unsigned alias = c % 3u;                              // 0 separate, 1 dst = a, 2 dst = b
                RwMatrix sa = random_matrix(rng), sb = random_matrix(rng), sd = random_matrix(rng);
                put(g, kA, sa);
                put(g, kB, sb);
                put(g, kD, sd);
                const u32 gd = alias == 1 ? kA : alias == 2 ? kB : kD;
                RwMatrix* nd = alias == 1 ? &sa : alias == 2 ? &sb : &sd;
                instructions += g.call(kMatrixMultiplyScalar.entry, {gd, kA, kB});
                MatrixMultiply(nd, &sa, &sb);
                same(g, kA, sa, true, "MatrixMultiply differs from the original (a)");
                same(g, kB, sb, true, "MatrixMultiply differs from the original (b)");
                same(g, kD, sd, true, "MatrixMultiply differs from the original (dst)");
            }
            // ---- RwMatrixUpdate, the three error measures, RwEngineSetMatrixTolerances, RwMatrixOptimize ------
            {
                RwMatrix u = random_matrix(rng);
                put(g, kD, u);
                instructions += g.call(kUpdate.entry, {kD});
                RwMatrixUpdate(&u);
                same(g, kD, u, true, "RwMatrixUpdate differs from the original");

                RwMatrix m = (c & 1u) ? near_rotation(rng) : random_matrix(rng);
                put(g, kA, m);
                instructions += g.call(kDeterminant.entry, {kA});
                require(bits(g.f1()) == bits(_rwMatrixDeterminant(&m)), "_rwMatrixDeterminant differs");
                instructions += g.call(kOrthogonalError.entry, {kA});
                require(bits(g.f1()) == bits(_rwMatrixOrthogonalError(&m)), "_rwMatrixOrthogonalError differs");
                instructions += g.call(kNormalError.entry, {kA});
                require(bits(g.f1()) == bits(_rwMatrixNormalError(&m)), "_rwMatrixNormalError differs");

                if (c % 64u == 0) {
                    const RwMatrixTolerance t{std::ldexp(1.0f, -static_cast<int>(rng.next() % 24u)),
                                              std::ldexp(1.0f, -static_cast<int>(rng.next() % 24u)),
                                              std::ldexp(1.0f, -static_cast<int>(rng.next() % 24u))};
                    g.write_float(kT, t.Normal);
                    g.write_float(kT + 4, t.Orthogonal);
                    g.write_float(kT + 8, t.Identity);
                    instructions += g.call(kSetTolerances.entry, {kT});
                    require(g.r3() == 1 && RwEngineSetMatrixTolerances(&t) == 1, "RwEngineSetMatrixTolerances");
                    require(g.memory().peek32(kEngine + kGuestMatrixOffset + 0xC) == bits(globals.tolerance.Normal) &&
                                g.memory().peek32(kEngine + kGuestMatrixOffset + 0x10) == bits(globals.tolerance.Orthogonal) &&
                                g.memory().peek32(kEngine + kGuestMatrixOffset + 0x14) == bits(globals.tolerance.Identity),
                            "the engine's tolerances differ");
                }
                const bool explicit_tolerance = rng.next() & 1u;
                const RwMatrixTolerance t{std::ldexp(1.0f, -static_cast<int>(rng.next() % 30u)),
                                          std::ldexp(1.0f, -static_cast<int>(rng.next() % 30u)),
                                          std::ldexp(1.0f, -static_cast<int>(rng.next() % 30u))};
                g.write_float(kT, t.Normal);
                g.write_float(kT + 4, t.Orthogonal);
                g.write_float(kT + 8, t.Identity);
                if (c % 5u == 0) {                                      // an identity, sometimes nudged
                    m.right = {1, 0, 0}, m.up = {0, 1, 0}, m.at = {0, 0, 1}, m.pos = {0, 0, 0};
                    if (rng.next() & 1u) m.pos.x = std::ldexp(1.0f, -static_cast<int>(rng.next() % 20u));
                    put(g, kA, m);
                }
                instructions += g.call(kOptimize.entry, {kA, explicit_tolerance ? kT : 0u});
                require(g.r3() == kA && RwMatrixOptimize(&m, explicit_tolerance ? &t : nullptr) == &m,
                        "RwMatrixOptimize must return matrix");
                same(g, kA, m, true, "RwMatrixOptimize differs from the original");
            }
            // ---- RwMatrixOrthoNormalize (separate and in place, zero axes sometimes) -------------------------
            {
                RwMatrix src = (c & 1u) ? near_rotation(rng) : random_matrix(rng);
                if (c % 7u == 0) (c % 3u == 0 ? src.right : c % 3u == 1 ? src.up : src.at) = {0, 0, 0};
                RwMatrix dst = random_matrix(rng);
                const bool in_place = c % 2u == 0;
                put(g, kA, src);
                put(g, kD, dst);
                const u32 gd = in_place ? kA : kD;
                RwMatrix* nd = in_place ? &src : &dst;
                instructions += g.call(kOrthoNormalize.entry, {gd, kA});
                require(g.r3() == gd && RwMatrixOrthoNormalize(nd, &src) == nd, "RwMatrixOrthoNormalize's result");
                same(g, kA, src, true, "RwMatrixOrthoNormalize differs from the original (src)");
                same(g, kD, dst, true, "RwMatrixOrthoNormalize differs from the original (dst)");
            }
            // ---- RwMatrixRotateOneMinusCosineSine and RwMatrixRotate ---------------------------------------
            {
                RwMatrix m = random_matrix(rng);
                const RwV3d axis{rng.real(-3, 3), rng.real(-3, 3), rng.real(-3, 3)};
                const float omc = rng.real(-1, 1), sine = rng.real(-1, 1);
                const RwOpCombineType op = combine_op(rng);
                put(g, kA, m);
                put(g, kV, axis);
                instructions += g.call(kRotateOmcs.entry, {kA, kV, static_cast<u32>(op)}, {double(omc), double(sine)});
                RwMatrix* r = RwMatrixRotateOneMinusCosineSine(&m, &axis, omc, sine, op);
                require((g.r3() == kA) == (r == &m) && (g.r3() == 0) == !valid_op(op),
                        "RwMatrixRotateOneMinusCosineSine's result");
                same(g, kA, m, !valid_op(op), "RwMatrixRotateOneMinusCosineSine differs from the original");
                same_error(g);
                if (!valid_op(op)) clear_error(g);

                RwMatrix n = random_matrix(rng);
                const float angle = static_cast<float>(static_cast<int>(rng.next() % 1440001u) - 720000) / 1000.0f;
                const RwOpCombineType op2 = combine_op(rng);
                put(g, kB, n);
                put(g, kV, axis);
                instructions += g.call(kRotate.entry, {kB, kV, static_cast<u32>(op2)}, {double(angle)});
                require(g.r3() == kB && RwMatrixRotate(&n, &axis, angle, op2) == &n, "RwMatrixRotate returns matrix");
                same(g, kB, n, !valid_op(op2), "RwMatrixRotate differs from the original");
                same_error(g);
                if (!valid_op(op2)) clear_error(g);
            }
            // ---- RwMatrixInvert (identity copy, orthonormal, general; separate only) ----------------------------
            // RenderWare forbids dst == src ("must not point to the same matrix"): the original then reads inputs it
            // has overwritten and returns a wrong inverse; the native returns the right one (rwcore_math_tests), so
            // the in-place results are not compared. No call in the game inverts in place (all 9 sites checked).
            {
                RwMatrix src = (c & 1u) ? near_rotation(rng) : random_matrix(rng, -6, 6);
                const u32 kind = c % 3u;
                src.flags = (src.flags & ~0x20003u) | (kind == 0 ? 0x20003u : kind == 1 ? 3u : (rng.next() & 1u));
                const RwInt32 opt = c % 11u == 0 ? 0 : rwMATRIXOPTIMIZE_IDENTITY;
                globals.matrixOptimizations = opt;
                g.memory().write32(kEngine + kGuestMatrixOffset + 0x4, static_cast<u32>(opt));
                RwMatrix dst = random_matrix(rng);
                put(g, kA, src);
                put(g, kD, dst);
                const u32 gd = kD;
                RwMatrix* nd = &dst;
                instructions += g.call(kInvert.entry, {gd, kA});
                require(g.r3() == gd && RwMatrixInvert(nd, &src) == nd, "RwMatrixInvert's result");
                same(g, kA, src, true, "RwMatrixInvert differs from the original (src)");
                same(g, kD, dst, true, "RwMatrixInvert differs from the original (dst)");
            }
            // ---- RwMatrixScale / RwMatrixTranslate / RwMatrixTransform -----------------------------------------
            for (int which = 0; which < 3; ++which) {
                RwMatrix m = random_matrix(rng);
                const RwMatrix t = random_matrix(rng);
                const RwV3d v{rng.real(), rng.real(), rng.real()};
                const RwOpCombineType op = combine_op(rng);
                put(g, kA, m);
                put(g, kB, t);
                put(g, kV, v);
                const u32 entry = which == 0 ? kScale.entry : which == 1 ? kTranslate.entry : kTransform.entry;
                instructions += g.call(entry, {kA, which == 2 ? kB : kV, static_cast<u32>(op)});
                RwMatrix* r = which == 0 ? RwMatrixScale(&m, &v, op)
                              : which == 1 ? RwMatrixTranslate(&m, &v, op)
                                           : RwMatrixTransform(&m, &t, op);
                require((g.r3() == kA) == (r == &m) && (g.r3() == 0) == !valid_op(op), "a combine function's result");
                // on an invalid op the original also writes to address 0xC (its NULL), which the native does not
                same(g, kA, m, which != 2 || op == rwCOMBINEREPLACE || !valid_op(op), "a combine function differs");
                same_error(g);
                if (!valid_op(op)) clear_error(g);
            }
            // ---- RwMatrixCreate / RwMatrixDestroy ---------------------------------------------------------------
            if (c % 16u == 0) {
                instructions += g.call(kCreate.entry, {});
                RwMatrix* m = RwMatrixCreate();
                require(g.r3() == alloc_at && m != nullptr, "RwMatrixCreate's result");
                same(g, alloc_at, *m, false, "RwMatrixCreate differs from the original");
                require(guest_allocs.back() == std::pair<u32, u32>{kGuestFreeList, rwMATRIXCREATEHINT} &&
                            g_allocs.back() == std::pair<RwFreeList*, RwUInt32>{kNativeFreeList, rwMATRIXCREATEHINT},
                        "RwMatrixCreate's allocation request differs");
                instructions += g.call(kDestroy.entry, {alloc_at});
                require(g.r3() == 1 && RwMatrixDestroy(m) == 1, "RwMatrixDestroy returns TRUE");
                require(guest_frees.back() == std::pair<u32, u32>{kGuestFreeList, alloc_at} &&
                            g_frees.back() == std::pair<RwFreeList*, void*>{kNativeFreeList, m},
                        "RwMatrixDestroy's free request differs");
            }
            // ---- RwV3dTransformPoint, separate and in place (kept from the first oracle) ------------------------
            {
                const RwMatrix m = random_matrix(rng);
                struct Pair { RwV3d in, out; RwUInt32 guard[2]; } pair{{rng.real(), rng.real(), rng.real()},
                                                                       {rng.real(), rng.real(), rng.real()},
                                                                       {rng.next(), rng.next()}};
                const bool in_place = c & 1u;
                put(g, kA, m);
                std::array<u32, 8> words{};
                std::memcpy(words.data(), &pair, sizeof pair);
                for (u32 i = 0; i < 8; ++i) g.memory().write32(kB + 4 * i, words[i]);
                const u32 out = in_place ? kB : kB + 12u;
                instructions += g.call(kTransformPoint.entry, {out, kB, kA});
                RwV3d* outp = in_place ? &pair.in : &pair.out;
                require(RwV3dTransformPoint(outp, &pair.in, &m) == outp, "RwV3dTransformPoint must return pointOut");
                std::memcpy(words.data(), &pair, sizeof pair);
                for (u32 i = 0; i < 8; ++i) require(g.memory().peek32(kB + 4 * i) == words[i], "RwV3dTransformPoint differs");
            }
            ++checks;
        }
        std::printf("rwcore_matrix_tests: %u cases x 19 functions bit-identical to the original (%llu original "
                    "instructions)\n", checks, static_cast<unsigned long long>(instructions));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_matrix_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
