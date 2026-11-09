// Oracle for the native RenderWare core vectors and errors (rwcore/src/vector.cpp, error.cpp): the ORIGINAL functions
// of PAL main.dol run in the reference interpreter in a guest RenderWare environment (rwcore_oracle.h); the native
// functions run on a native 64-bit engine instance; every result, every table word, every allocation request and
// every free is compared.
// liveness-roots: reference-interpreter-only
//
// Input domain (stated): _rwSqrt / _rwInvSqrt take any float whose bits are zero or a normal number (the lookup is
// integer arithmetic on the bits); RwV2dLength, RwV3dLength, RwV3dNormalize and the transforms take finite floats from 2^-20 to 2^20 in
// magnitude, both signs, and +-0. NaN payloads and subnormals are outside it.
//
// Usage: rwcore_vector_tests <main.dol>
#include "rw/core/error.h"
#include "rw/core/vector.h"
#include "rwcore_oracle.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
using namespace rworacle;

constexpr Original kErrorOpen{"_rwErrorOpen", 0x8047EB88u, 0x38u, 0x390D6100u, 0x4E800020u};
constexpr Original kErrorClose{"_rwErrorClose", 0x8047EBC0u, 0x14u, 0x38AD6100u, 0x4E800020u};
constexpr Original kErrorSet{"RwErrorSet", 0x8047EBD4u, 0x60u, 0x808D615Cu, 0x4E800020u};
constexpr Original kErrorGet{"RwErrorGet", 0x8047EC34u, 0x44u, 0x808D615Cu, 0x4E800020u};
constexpr Original kError{"_rwerror", 0x8047EC78u, 0x68u, 0x9421FF80u, 0x4E800020u};
constexpr Original kSqrt{"_rwSqrt", 0x80483390u, 0x54u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kInvSqrt{"_rwInvSqrt", 0x804833E4u, 0x5Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kV2dLength{"RwV2dLength", 0x80483440u, 0x68u, 0xC0230000u, 0x4E800020u};
constexpr Original kV3dNormalize{"RwV3dNormalize", 0x8048327Cu, 0xD4u, 0x9421FFD0u, 0x4E800020u};
constexpr Original kV3dLength{"RwV3dLength", 0x80483350u, 0x40u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kTransformPoint{"RwV3dTransformPoint", 0x804834A8u, 0x4Cu, 0xE1040000u, 0x4E800020u};
constexpr Original kTransformPoints{"RwV3dTransformPoints", 0x804834F4u, 0x60u, 0x7CA903A6u, 0x4E800020u};
constexpr Original kTransformVector{"RwV3dTransformVector", 0x80483554u, 0x44u, 0xE0C40000u, 0x4E800020u};
constexpr Original kVectorClose{"_rwVectorClose", 0x80483598u, 0xA4u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kVectorOpen{"_rwVectorOpen", 0x8048363Cu, 0x398u, 0x9421FFB0u, 0x4E800020u};

constexpr u32 kGuestErrorOffset = 0x200u, kGuestVectorOffset = 0x210u;

// ---- the native engine: RwGlobals + the error and vector modules, memory functions that record their calls ----
struct NativeCall {
    std::size_t size;
    RwUInt32 hint;
};
std::vector<NativeCall> g_mallocs;
std::vector<void*> g_frees;
int g_fail_call = -1;

void* recordingMalloc(std::size_t size, RwUInt32 hint) {
    g_mallocs.push_back({size, hint});
    if (static_cast<int>(g_mallocs.size()) - 1 == g_fail_call) return nullptr;
    return std::malloc(size);
}
void recordingFree(void* mem) {
    g_frees.push_back(mem);
    std::free(mem);
}

struct NativeEngine {
    static constexpr RwInt32 kErrorOffset = static_cast<RwInt32>((sizeof(RwGlobals) + 15) & ~std::size_t{15});
    static constexpr RwInt32 kVectorOffset = kErrorOffset + 16;
    std::vector<std::max_align_t> block = std::vector<std::max_align_t>(64 + sizeof(RwGlobals) / sizeof(std::max_align_t));

    NativeEngine() {
        RwEngineInstance = block.data();
        rwSrcGlobals() = RwGlobals{};
        RwMemoryFunctions functions = rwHostMemoryFunctions();
        functions.rwmalloc = recordingMalloc;
        functions.rwfree = recordingFree;
        rwSrcGlobals().memoryFuncs = functions;
        g_mallocs.clear();
        g_frees.clear();
        g_fail_call = -1;
        errorModule = {};
        vectorModule = {};
    }
};

void reset_guest_modules(Guest& g) {
    g.reset_engine();
    for (u32 a : {kErrorModule, kErrorModule + 4u, kVectorModule, kVectorModule + 4u}) g.memory().write32(a, 0);
}

void same_error_state(Guest& g) {
    const RwError& held = rwModuleGlobals<RwError>(errorModule);
    require(g.memory().peek32(kEngine + kGuestErrorOffset) == static_cast<u32>(held.pluginID) &&
                g.memory().peek32(kEngine + kGuestErrorOffset + 4) == static_cast<u32>(held.errorCode),
            "the held error differs from the original's");
    require(g.memory().peek32(kErrorModule + 4) == static_cast<u32>(errorModule.numInstances),
            "errorModule.numInstances differs from the original's");
}

void same_allocations(const Guest& g) {
    require(g.mallocs.size() == g_mallocs.size(), "a different number of rwmalloc calls");
    for (std::size_t i = 0; i < g_mallocs.size(); ++i) {
        require(g.mallocs[i].size == g_mallocs[i].size && g.mallocs[i].hint == g_mallocs[i].hint,
                "an rwmalloc call differs (size or hint)");
    }
}

void open_both(Guest& g, bool vector) {
    g.call(kErrorOpen.entry, {kEngine, kGuestErrorOffset, 8});
    require(g.r3() == kEngine, "_rwErrorOpen must return the instance");
    require(_rwErrorOpen(RwEngineInstance, NativeEngine::kErrorOffset, 8) == RwEngineInstance,
            "native _rwErrorOpen must return the instance");
    same_error_state(g);
    if (vector) {
        g.call(kVectorOpen.entry, {kEngine, kGuestVectorOffset, 8});
        void* native = _rwVectorOpen(RwEngineInstance, NativeEngine::kVectorOffset, 8);
        require((g.r3() == kEngine) == (native == RwEngineInstance), "_rwVectorOpen's result differs");
    }
}

void same_tables(Guest& g) {
    const rwVectorGlobals& globals = rwModuleGlobals<rwVectorGlobals>(vectorModule);
    const u32 sqrt_tab = g.memory().peek32(kEngine + kGuestVectorOffset);
    const u32 inv_tab = g.memory().peek32(kEngine + kGuestVectorOffset + 4);
    require((sqrt_tab != 0) == (globals.SqrtTab != nullptr) && (inv_tab != 0) == (globals.InvSqrtTab != nullptr),
            "a table exists on one side only");
    for (u32 i = 0; i < 4096; ++i) {
        if (sqrt_tab) require(g.memory().peek32(sqrt_tab + 4 * i) == globals.SqrtTab[i], "a SqrtTab entry differs");
        if (inv_tab) require(g.memory().peek32(inv_tab + 4 * i) == globals.InvSqrtTab[i], "an InvSqrtTab entry differs");
    }
    require(g.memory().peek32(kVectorModule + 4) == static_cast<u32>(vectorModule.numInstances),
            "vectorModule.numInstances differs from the original's");
}

void write_matrix(Guest& g, u32 a, const RwMatrix& m) {
    std::array<u32, 16> words{};                                   // RwMatrix holds no pointers: 16 words
    std::memcpy(words.data(), &m, sizeof m);
    for (u32 i = 0; i < 16; ++i) g.memory().write32(a + 4 * i, words[i]);
}

RwMatrix random_matrix(Rng& rng) {
    RwMatrix m{};
    for (RwV3d* row : {&m.right, &m.up, &m.at, &m.pos}) *row = {rng.real(), rng.real(), rng.real()};
    m.flags = rng.next();
    m.pad1 = rng.next();
    m.pad2 = rng.next();
    m.pad3 = rng.next();
    return m;
}

u32 bits(float v) { return std::bit_cast<u32>(v); }

/// A float whose bits are zero or a normal number.
float lookup_input(Rng& rng) {
    const u32 r = rng.next();
    if ((r & 255u) == 0) return 0.0f;
    const u32 exponent = 1u + rng.next() % 254u;
    return std::bit_cast<float>((r & 0x80000000u) | (exponent << 23) | (rng.next() & 0x7FFFFFu));
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rwcore_vector_tests main.dol");
        Guest g(argv[1]);
        for (const Original& f : {kErrorOpen, kErrorClose, kErrorSet, kErrorGet, kError, kSqrt, kInvSqrt, kV2dLength,
                                  kV3dNormalize, kV3dLength, kTransformPoint, kTransformPoints, kTransformVector, kVectorClose, kVectorOpen}) {
            g.check_pinned(f);
        }
        Rng rng;
        u64 instructions = 0;
        unsigned checks = 0;

        // ---- errors: open, then set / get sequences --------------------------------------------------------
        {
            NativeEngine native;
            reset_guest_modules(g);
            open_both(g, false);
            for (unsigned i = 0; i < 4000; ++i) {
                if (rng.next() % 3u != 0) {
                    RwError code{static_cast<RwInt32>(rng.next() % 5u),
                                 static_cast<RwInt32>((rng.next() & 1u ? 0x80000000u : 0u) | (rng.next() & 0xFFu))};
                    g.memory().write32(kBuffer, static_cast<u32>(code.pluginID));
                    g.memory().write32(kBuffer + 4, static_cast<u32>(code.errorCode));
                    instructions += g.call(kErrorSet.entry, {kBuffer});
                    require(g.r3() == kBuffer && RwErrorSet(&code) == &code, "RwErrorSet must return code");
                } else {
                    RwError out{};
                    instructions += g.call(kErrorGet.entry, {kBuffer + 8});
                    require(g.r3() == kBuffer + 8 && RwErrorGet(&out) == &out, "RwErrorGet must return code");
                    require(g.memory().peek32(kBuffer + 8) == static_cast<u32>(out.pluginID) &&
                                g.memory().peek32(kBuffer + 12) == static_cast<u32>(out.errorCode),
                            "RwErrorGet returned a different error");
                }
                same_error_state(g);
                ++checks;
            }
            for (unsigned i = 0; i < 200; ++i) {
                const u32 code = rng.next();
                instructions += g.call(kError.entry, {code, rng.next(), rng.next()});
                require(g.r3() == code && static_cast<u32>(_rwerror(static_cast<RwInt32>(code), 1, 2)) == code,
                        "_rwerror must return its code");
            }
            instructions += g.call(kErrorClose.entry, {kEngine, kGuestErrorOffset, 8});
            require(g.r3() == kEngine && _rwErrorClose(RwEngineInstance, NativeEngine::kErrorOffset, 8) == RwEngineInstance,
                    "_rwErrorClose must return the instance");
            same_error_state(g);
        }

        // ---- the vector module: open (tables), lookups, transforms, close ------------------------------------
        {
            NativeEngine native;
            reset_guest_modules(g);
            open_both(g, true);
            same_tables(g);
            same_allocations(g);
            same_error_state(g);
            checks += 8192;

            for (unsigned i = 0; i < 100000; ++i) {
                const float x = lookup_input(rng);
                instructions += g.call(kSqrt.entry, {}, {double(x)});
                require(bits(g.f1()) == bits(_rwSqrt(x)), "_rwSqrt differs from the original");
                instructions += g.call(kInvSqrt.entry, {}, {double(x)});
                require(bits(g.f1()) == bits(_rwInvSqrt(x)), "_rwInvSqrt differs from the original");
                const RwV2d v{rng.real(), rng.real()};
                g.write_float(kBuffer, v.x);
                g.write_float(kBuffer + 4, v.y);
                instructions += g.call(kV2dLength.entry, {kBuffer});
                require(bits(g.f1()) == bits(RwV2dLength(&v)), "RwV2dLength differs from the original");
                // RwV3dLength / RwV3dNormalize, in place or separate, with zero vectors now and then
                RwV3d w{rng.real(), rng.real(), rng.real()};
                if (i % 97u == 0) w = {i % 2u ? -0.0f : 0.0f, 0.0f, 0.0f};
                const bool in_place = i % 2u == 0;
                g.write_float(kBuffer + 0x10, w.x);
                g.write_float(kBuffer + 0x14, w.y);
                g.write_float(kBuffer + 0x18, w.z);
                instructions += g.call(kV3dLength.entry, {kBuffer + 0x10});
                require(bits(g.f1()) == bits(RwV3dLength(&w)), "RwV3dLength differs from the original");
                RwV3d normal{};
                const u32 out = in_place ? kBuffer + 0x10 : kBuffer + 0x20;
                instructions += g.call(kV3dNormalize.entry, {out, kBuffer + 0x10});
                const RwReal length = in_place ? RwV3dNormalize(&w, &w) : RwV3dNormalize(&normal, &w);
                const RwV3d& n = in_place ? w : normal;
                require(bits(g.f1()) == bits(length) && g.memory().peek32(out) == bits(n.x) &&
                            g.memory().peek32(out + 4) == bits(n.y) && g.memory().peek32(out + 8) == bits(n.z),
                        "RwV3dNormalize differs from the original");
                same_error_state(g);
                RwError taken{};
                instructions += g.call(kErrorGet.entry, {kBuffer + 0x40});      // clear it, so each zero vector counts
                RwErrorGet(&taken);
                checks += 5;
            }

            constexpr u32 kMatrix = kBuffer + 0x100u, kPoints = kBuffer + 0x200u;
            for (unsigned c = 0; c < 20000; ++c) {
                const RwMatrix m = random_matrix(rng);
                write_matrix(g, kMatrix, m);
                std::array<RwV3d, 24> native_points{};
                for (unsigned i = 0; i < native_points.size(); ++i) {
                    native_points[i] = {rng.real(), rng.real(), rng.real()};
                    g.write_float(kPoints + 12 * i, native_points[i].x);
                    g.write_float(kPoints + 12 * i + 4, native_points[i].y);
                    g.write_float(kPoints + 12 * i + 8, native_points[i].z);
                }
                // in at 8, out at 8 (in place), 9 (one ahead), 7 (one behind) or 16 (separate); the vector
                // transform in place or separate
                const unsigned mode = c % 5u;
                const RwInt32 count = 1 + static_cast<RwInt32>(rng.next() % 6u);
                const unsigned in = 8;
                const unsigned out = mode == 0 ? 8 : mode == 1 ? 9 : mode == 2 ? 7 : mode == 3 ? 16 : (c / 5u) % 2u ? 8 : 16;
                if (mode == 4) {
                    instructions += g.call(kTransformVector.entry, {kPoints + 12 * out, kPoints + 12 * in, kMatrix});
                    RwV3d* r = RwV3dTransformVector(&native_points[out], &native_points[in], &m);
                    require(g.r3() == kPoints + 12 * out && r == &native_points[out], "RwV3dTransformVector's result");
                } else if (c % 7u == 0) {
                    instructions += g.call(kTransformPoint.entry, {kPoints + 12 * out, kPoints + 12 * in, kMatrix});
                    RwV3d* r = RwV3dTransformPoint(&native_points[out], &native_points[in], &m);
                    require(g.r3() == kPoints + 12 * out && r == &native_points[out], "RwV3dTransformPoint's result");
                } else {
                    instructions += g.call(kTransformPoints.entry,
                                           {kPoints + 12 * out, kPoints + 12 * in, static_cast<u32>(count), kMatrix});
                    RwV3d* r = RwV3dTransformPoints(&native_points[out], &native_points[in], count, &m);
                    require(g.r3() == kPoints + 12 * out && r == &native_points[out], "RwV3dTransformPoints' result");
                }
                for (unsigned i = 0; i < native_points.size(); ++i) {
                    require(g.memory().peek32(kPoints + 12 * i) == bits(native_points[i].x) &&
                                g.memory().peek32(kPoints + 12 * i + 4) == bits(native_points[i].y) &&
                                g.memory().peek32(kPoints + 12 * i + 8) == bits(native_points[i].z),
                            "a transformed point differs from the original");
                }
                ++checks;
            }

            const u32 guest_sqrt = g.memory().peek32(kEngine + kGuestVectorOffset);
            const u32 guest_inv = g.memory().peek32(kEngine + kGuestVectorOffset + 4);
            const rwVectorGlobals tables = rwModuleGlobals<rwVectorGlobals>(vectorModule);
            instructions += g.call(kVectorClose.entry, {kEngine, kGuestVectorOffset, 8});
            require(g.r3() == kEngine && _rwVectorClose(RwEngineInstance, NativeEngine::kVectorOffset, 8) == RwEngineInstance,
                    "_rwVectorClose must return the instance");
            require(g.frees == std::vector<u32>{guest_inv, guest_sqrt}, "the original frees the inverse table first");
            require(g_frees == std::vector<void*>{tables.InvSqrtTab, tables.SqrtTab}, "native frees in another order");
            same_tables(g);
        }

        // ---- the vector module when an allocation fails: the first, then the second table -------------------
        for (int fail : {0, 1}) {
            NativeEngine native;
            reset_guest_modules(g);
            open_both(g, false);
            g.fail_call = fail;
            g_fail_call = fail;
            instructions += g.call(kVectorOpen.entry, {kEngine, kGuestVectorOffset, 8});
            void* r = _rwVectorOpen(RwEngineInstance, NativeEngine::kVectorOffset, 8);
            require(g.r3() == 0 && r == nullptr, "_rwVectorOpen must fail when an allocation fails");
            if (fail == 1) {
                // the second table failed: the original keeps (leaks) the first, the native frees it - a deliberate
                // difference (no close follows a failed open); the requests made are still the same
                require(g.memory().peek32(kEngine + kGuestVectorOffset) != 0 && g.frees.empty(), "the original keeps the first table");
                const rwVectorGlobals& tables = rwModuleGlobals<rwVectorGlobals>(vectorModule);
                require(tables.SqrtTab == nullptr && tables.InvSqrtTab == nullptr && g_frees.size() == 1,
                        "the native must free the first table");
                g_frees.clear();
            } else {
                same_tables(g);
            }
            same_allocations(g);
            same_error_state(g);
            checks += 3;
        }

        std::printf("rwcore_vector_tests: %u checks bit-identical to the original (%llu original instructions)\n",
                    checks, static_cast<unsigned long long>(instructions));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_vector_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
