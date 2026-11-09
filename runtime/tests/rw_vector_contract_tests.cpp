// liveness-roots: reference-interpreter-only
// Original-DOL contract for the native RenderWare vector transforms (gekko/native_rw_vector.h): RwV3dTransformPoint 0x804834A8,
// RwV3dTransformPoints 0x804834F4 and RwV3dTransformVector 0x80483554, and the square-root family: _rwSqrt 0x80483390,
// _rwInvSqrt 0x804833E4, RwV3dLength 0x80483350, RwV2dLength 0x80483440 and RwV3dNormalize 0x8048327C (the engine's lookup
// tables are filled with random words: the contract is the lookup, not the table; f1 is compared for those entries and the
// second slot of f14..f31 is compared everywhere; RwV3dNormalize's error path is the guest body's, run by both sides).
//
// Reference interpreter, pinned DOL. Each case builds one memory state (a vector or point array, a matrix, an output area
// that may overlap them) and runs the ORIGINAL paired-single body and the owner from byte-identical memory, then compares every
// byte of a 4 KiB window, r3, r1, r2, r13, r14..r31, LR and f14..f31. The volatile registers (f0..f13, r4..r12, CTR, CR) are
// not compared: nothing reads them after a leaf call (docs/NATIVE_MATH_METHOD.md). Inputs are random bit patterns drawn
// from every float class: normals of both signs, zeros, subnormals, values near the products' underflow boundary, infinities,
// quiet and signalling NaNs with payloads, extremes. Both FPSCR[NI] settings, aliasing (out = in, out one element past
// in, out inside the matrix), and array counts 1..9 are covered.
//
// Negative controls: owners that use an unfused multiply-add, that flush when they should not, that read the next element
// after storing, or that swap the rows are told apart.
//
// usage: rw_vector_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_rw_vector.h"
#include "gekko/ppc_ops.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
namespace rv = gekko::rw_vector;

constexpr u32 kSda = 0x805EC500u, kSda2 = 0x805FA780u;
constexpr u32 kStack = 0x817E0000u, kFinish = 0x817FF000u;
constexpr u32 kWindow = 0x81302000u, kWindowBytes = 0x1000u;
constexpr u32 kIn = 0x81302100u, kMatrix = 0x81302400u, kOut = 0x81302600u;
constexpr u32 kEngine = 0x805E3C08u, kModuleOffset = 0x300u, kSqrtTable = 0x81310000u, kInvTable = 0x81320000u;

void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

struct Rng {
    u32 state;
    explicit Rng(u32 seed) : state(seed * 2654435761u + 12345u) {}
    u32 next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8 | (state << 24);
    }
    u32 below(u32 n) { return next() % n; }
};

// A float bit pattern from a mixture of classes.
u32 random_float(Rng& rng) {
    const u32 sign = rng.below(2) ? 0x80000000u : 0u;
    switch (rng.below(14)) {
        case 0: return sign;                                              // +-0
        case 1: return sign | (rng.next() & 0x007FFFFFu);                 // subnormal
        case 2: return sign | 0x7F800000u;                                // inf
        case 3: return sign | 0x7F800000u | (rng.next() & 0x007FFFFFu) | 1u;       // NaN (quiet or signalling)
        case 4: return sign | 0x7FC00000u | (rng.next() & 0x003FFFFFu);   // quiet NaN
        case 5: return sign | 0x00800000u | (rng.next() & 0x000FFFFFu);   // near the smallest normals
        case 6: return sign | 0x7F7FFFFFu;                                // max
        case 7: return sign | ((0x3F000000u - 0x0C000000u) + (rng.next() & 0x00FFFFFFu));  // ~2^-60: products near 2^-120
        case 8: return sign | ((0x3F800000u - 0x1F000000u) + (rng.next() & 0x00FFFFFFu));  // ~2^-62
        case 9: return sign | (0x3F800000u + (rng.next() & 0x007FFFFFu)); // [1,2)
        default: return sign | ((0x3C000000u + (rng.next() & 0x0FFFFFFFu)) & 0x7F7FFFFFu);  // ordinary range
    }
}

struct Snapshot {
    u32 r3, r1, r2, r13, lr;
    u32 saved[18];
    u64 f[18], f_second[18], f1;
    std::vector<u8> window;
};

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};

    bool has_f1 = false;
    u64 f1_value = 0;

    Machine(const char* path, bool native, const std::function<void(Cpu&)>& wrong = nullptr) {
        std::ifstream input(path, std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        unsigned long long hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        // SHA-256: fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af.
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(static_cast<unsigned long long>(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
        if (native) {
            rv::install(cpu);
            if (wrong) wrong(cpu);
        }
    }

    void w(u32 address, u32 value) { memory.write32(address, value); }

    void call(u32 entry, u32 r3, u32 r4, u32 r5, u32 r6, bool ni) {
        cpu.reset();
        cpu.set_msr(cpu.msr() | 0x2000u);  // FP available
        cpu.spr[SPR_GQR0] = 0u;
        cpu.fpscr = ni ? kFpscrNI : 0u;
        cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = kFinish;
        for (unsigned i = 0; i < 32; ++i) cpu.gpr[i] = 0x1000u + i * 0x111u;
        cpu.gpr[1] = kStack; cpu.gpr[2] = kSda2; cpu.gpr[13] = kSda;
        cpu.gpr[3] = r3; cpu.gpr[4] = r4; cpu.gpr[5] = r5; cpu.gpr[6] = r6;
        for (unsigned i = 0; i < 32; ++i) {
            const u64 value = 0x4000000000000000ull + i;
            std::memcpy(&cpu.fpr[i].bits[0], &value, sizeof(value));
            std::memcpy(&cpu.fpr[i].bits[1], &value, sizeof(value));
        }
        if (has_f1) cpu.fpr[1].bits[0] = cpu.fpr[1].bits[1] = f1_value;
        unsigned steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 400000) cpu.run_steps(1);
        require(cpu.pc == kFinish && !cpu.stopped(), "the call did not return");
    }

    Snapshot snapshot() const {
        Snapshot s{};
        s.r3 = cpu.gpr[3]; s.r1 = cpu.gpr[1]; s.r2 = cpu.gpr[2]; s.r13 = cpu.gpr[13]; s.lr = cpu.lr;
        for (unsigned i = 0; i < 18; ++i) {
            s.saved[i] = cpu.gpr[14 + i];
            std::memcpy(&s.f[i], &cpu.fpr[14 + i].bits[0], sizeof(u64));
            std::memcpy(&s.f_second[i], &cpu.fpr[14 + i].bits[1], sizeof(u64));
        }
        std::memcpy(&s.f1, &cpu.fpr[1].bits[0], sizeof(u64));
        s.window.resize(kWindowBytes);
        for (u32 i = 0; i < kWindowBytes; ++i) s.window[i] = memory.peek8(kWindow + i);
        return s;
    }
};

std::string diff(const Snapshot& a, const Snapshot& b, bool compare_f1) {
    char text[120];
    // r3 is a scratch register in the square-root family (the original leaves a table pointer there); the result is f1
    if (!compare_f1 && a.r3 != b.r3) { std::snprintf(text, sizeof text, "r3 original=%08X native=%08X", a.r3, b.r3); return text; }
    if (a.r1 != b.r1 || a.r2 != b.r2 || a.r13 != b.r13 || a.lr != b.lr) return "r1 / r2 / r13 / lr differ";
    if (std::memcmp(a.saved, b.saved, sizeof a.saved) != 0) return "a callee-saved register differs";
    if (std::memcmp(a.f, b.f, sizeof a.f) != 0) return "a callee-saved FPR differs";
    if (std::memcmp(a.f_second, b.f_second, sizeof a.f_second) != 0) return "a callee-saved FPR second slot differs";
    if (compare_f1 && a.f1 != b.f1) {
        std::snprintf(text, sizeof text, "f1 original=%016llX native=%016llX", static_cast<unsigned long long>(a.f1),
                      static_cast<unsigned long long>(b.f1));
        return text;
    }
    for (u32 i = 0; i < kWindowBytes; i += 4) {
        u32 x, y;
        std::memcpy(&x, &a.window[i], 4);
        std::memcpy(&y, &b.window[i], 4);
        if (x != y) { std::snprintf(text, sizeof text, "window byte 0x%08X differs (original %08X native %08X)", kWindow + i, x, y); return text; }
    }
    return {};
}

unsigned g_cases = 0, g_skipped = 0;
bool g_sqrt_only = false;  // the negative controls of the square-root family skip the transform cases
const std::function<void(Cpu&)>* g_wrong = nullptr;

struct Case {
    u32 entry;
    u32 out, in, count, matrix;  // count 0 for the single-element entries
    bool ni;
    u32 seed;
    u64 f1 = 0;
    bool uses_f1 = false;  // the entry takes / returns f1 (the square-root family)
};

bool g_tables = false;  // the square-root tables are only built for the square-root family

void fill(Machine& m, u32 seed) {
    for (u32 a = kWindow; a < kWindow + kWindowBytes; a += 4) m.w(a, 0x5A5A5A5Au ^ (a * 40503u));
    Rng rng(seed);
    for (u32 a = kIn; a < kIn + 0x300u; a += 4) m.w(a, random_float(rng));
    for (u32 a = kMatrix; a < kMatrix + 0x40u; a += 4) m.w(a, random_float(rng));
    // the engine's square-root tables, reached through the engine instance
    m.w(kSda + 0x615Cu, kEngine);
    m.w(kSda + 0x6108u, kModuleOffset);
    m.w(kSda + 0x6140u, kModuleOffset);
    m.w(kEngine + kModuleOffset, kSqrtTable);
    m.w(kEngine + kModuleOffset + 4u, kInvTable);
    for (u32 a = 0; g_tables && a < 0x4000u; a += 4) {
        m.w(kSqrtTable + a, rng.next());
        m.w(kInvTable + a, rng.next());
    }
}

void compare(const char* path, const char* what, const Case& k) {
    Machine original(path, false), native(path, true, g_wrong ? *g_wrong : std::function<void(Cpu&)>{});
    g_tables = k.uses_f1;
    fill(original, k.seed);
    fill(native, k.seed);
    if (k.uses_f1) {
        original.has_f1 = native.has_f1 = true;
        original.f1_value = native.f1_value = k.f1;
    }
    bool original_ok = true, native_ok = true;
    const auto run = [&](Machine& m, bool& ok) {
        try {
            if (k.entry == rv::kTransformPoints) m.call(k.entry, k.out, k.in, k.count, k.matrix, k.ni);
            else m.call(k.entry, k.out, k.in, k.matrix, 0u, k.ni);
        } catch (const std::exception&) {
            ok = false;
        }
    };
    run(original, original_ok);
    run(native, native_ok);
    if (original_ok != native_ok) throw std::runtime_error(std::string(what) + ": one side returned and the other did not");
    if (!original_ok) {  // RwV3dNormalize's error path runs the engine's error handler, which the test machine does not hold
        ++g_skipped;
        return;
    }
    const std::string d = diff(original.snapshot(), native.snapshot(), k.uses_f1);
    if (!d.empty()) {
        if (std::getenv("RW_VECTOR_TRACE") != nullptr) {
            std::fprintf(stderr, "in:");
            for (u32 i = 0; i < 3; ++i) std::fprintf(stderr, " %08X", native.memory.peek32(k.in + 4 * i));
            std::fprintf(stderr, "\nmatrix:");
            for (u32 i = 0; i < 16; ++i) std::fprintf(stderr, "%s%08X", i % 4 == 0 ? "\n  " : " ", native.memory.peek32(k.matrix + 4 * i));
            std::fprintf(stderr, "\nout orig:");
            for (u32 i = 0; i < 3; ++i) std::fprintf(stderr, " %08X", original.memory.peek32(k.out + 4 * i));
            std::fprintf(stderr, "\nout nat :");
            for (u32 i = 0; i < 3; ++i) std::fprintf(stderr, " %08X", native.memory.peek32(k.out + 4 * i));
            std::fprintf(stderr, "\n");
        }
        char text[220];
        std::snprintf(text, sizeof text, "%s (seed %u, ni %d, out %08X in %08X count %u matrix %08X): %s", what, k.seed,
                      k.ni ? 1 : 0, k.out, k.in, k.count, k.matrix, d.c_str());
        throw std::runtime_error(text);
    }
    ++g_cases;
}

void run_all(const char* path) {
    g_cases = 0;
    u32 seed = 1;
    for (const bool ni : {false, true}) {
        if (g_sqrt_only) continue;
        // single point / vector: separate buffers, out == in, out overlapping the next component, out inside the matrix
        for (const u32 entry : {rv::kTransformPoint, rv::kTransformVector})
            for (const u32 out : {kOut, kIn, kIn + 4u, kIn + 8u, kMatrix + 8u, kMatrix + 0x30u})
                for (u32 repeat = 0; repeat < 40; ++repeat)
                    compare(path, entry == rv::kTransformPoint ? "transform point" : "transform vector",
                            Case{entry, out, kIn, 0u, kMatrix, ni, seed++});
        // arrays: counts 1..9, separate, in place, out one element after / before in, out overlapping the matrix
        for (u32 count = 1; count <= 9; ++count)
            for (const u32 out : {kOut, kIn, kIn + 12u, kIn + 4u, kMatrix - 12u * count / 2u})
                for (u32 repeat = 0; repeat < 8; ++repeat)
                    compare(path, "transform points", Case{rv::kTransformPoints, out, kIn, count, kMatrix, ni, seed++});
    }
    // the square-root family
    for (const bool ni : {false, true}) {
        Rng rng(seed + 7000u);
        for (const u32 entry : {rv::kRwSqrt, rv::kRwInvSqrt})
            for (u32 repeat = 0; repeat < 240; ++repeat) {
                const u64 value = repeat % 3 == 0   ? (static_cast<u64>(rng.next()) << 32 | rng.next())  // any double
                                  : repeat % 3 == 1 ? single_to_double_bits(random_float(rng))           // a widened single
                                                    : single_to_double_bits(0x3F000000u + rng.below(0x01000000u));
                compare(path, entry == rv::kRwSqrt ? "rwSqrt" : "rwInvSqrt",
                        Case{entry, 0u, kIn, 0u, kMatrix, ni, seed++, value, true});
            }
        for (const u32 entry : {rv::kV3dLength, rv::kV2dLength})
            for (u32 repeat = 0; repeat < 100; ++repeat)
                compare(path, entry == rv::kV3dLength ? "v3d length" : "v2d length",
                        Case{entry, kIn, kIn, 0u, kMatrix, ni, seed++, 0u, true});
        for (const u32 out : {kOut, kIn, kIn + 4u, kIn + 8u})
            for (u32 repeat = 0; repeat < 60; ++repeat)
                compare(path, "v3d normalize", Case{rv::kV3dNormalize, out, kIn, 0u, kMatrix, ni, seed++, 0u, true});
    }
}

bool caught(const char* path, const std::function<void(Cpu&)>& wrong, bool sqrt_only = false) {
    g_wrong = &wrong;
    g_sqrt_only = sqrt_only;
    bool found = false;
    try {
        run_all(path);
    } catch (const std::exception&) {
        found = true;
    }
    g_wrong = nullptr;
    g_sqrt_only = false;
    return found;
}

// A wrong owner of the square-root family: kind 3 = _rwSqrt without the rounding bias, kind 4 = RwV3dLength without the z term.
void install_wrong_sqrt(Cpu& cpu, int kind) {
    if (kind == 3)
        cpu.install_hle(rv::kRwSqrt, [](Cpu& c) {
            const u32 word = double_to_single_bits(c.fpr[1].bits[0]);
            if (word == 0u) return rv::serve_rw_sqrt(c);
            const u32 engine = c.mem_read32(c.gpr[13] + 0x615Cu);
            const u32 table = c.mem_read32(engine + c.mem_read32(c.gpr[13] + 0x6140u));
            const u32 rotated = (word << 22 | word >> 10) & 0x3FFCu;
            const u32 result = ((word >> 1 | word << 31) & 0x3FC00000u) + c.mem_read32(table + rotated);
            c.fpr[1].bits[0] = c.fpr[1].bits[1] = single_to_double_bits(result);
            return true;
        }, "wrong rwSqrt");
    if (kind == 4)
        cpu.install_hle(rv::kV3dLength, [](Cpu& c) {
            const u32 v = c.gpr[3];
            const u64 x = single_to_double_bits(c.mem_read32(v)), y = single_to_double_bits(c.mem_read32(v + 4u)),
                      z = single_to_double_bits(c.mem_read32(v + 8u));
            (void)z;  // the sqrt lookup keys on the top ~12 bits of the sum, so a last-bit rounding change is invisible: drop z instead
            c.fpr[1].bits[0] = ppc::fps_add(c, ppc::fps_mul(c, x, x), ppc::fps_mul(c, y, y));
            return rv::serve_rw_sqrt(c);
        }, "wrong v3d length");
}

// A deliberately wrong owner of the point transform for the negative controls.
void install_wrong_point(Cpu& cpu, int kind) {
    cpu.install_hle(rv::kTransformPoint, [kind](Cpu& c) {
        auto load = [&](u32 a) { u32 w = c.mem_read32(a); float f; std::memcpy(&f, &w, 4); return f; };
        float p[3] = {load(c.gpr[4]), load(c.gpr[4] + 4), load(c.gpr[4] + 8)};
        float out[3];
        for (u32 k = 0; k < 3; ++k) {
            const float right = load(c.gpr[5] + 4 * k), up = load(c.gpr[5] + 16 + 4 * k), at = load(c.gpr[5] + 32 + 4 * k),
                        pos = load(c.gpr[5] + 48 + 4 * k);
            if (kind == 0) out[k] = at * p[2] + (up * p[1] + (right * p[0] + pos));         // unfused
            else if (kind == 1) out[k] = std::fma(right, p[0], std::fma(up, p[1], std::fma(at, p[2], pos)));  // wrong order
            else out[k] = std::fma(at, p[2], std::fma(up, p[1], std::fma(right, p[0], pos)));
        }
        for (u32 k = 0; k < 3; ++k) {
            u32 w;
            std::memcpy(&w, &out[k], 4);
            c.mem_write32(c.gpr[3] + 4 * k, w);
        }
        return true;
    }, "wrong point transform");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rw_vector_contract_tests main.dol");
        run_all(argv[1]);
        const unsigned cases = g_cases;
        // count 0 stops by name; a disabled FPU declines (the guest body runs, so the original's result is the owner's)
        {
            Machine native(argv[1], true);
            fill(native, 1);
            native.cpu.reset();
            native.cpu.set_msr(native.cpu.msr() | 0x2000u);
            native.cpu.pc = rv::kTransformPoints; native.cpu.npc = rv::kTransformPoints + 4; native.cpu.lr = kFinish;
            native.cpu.gpr[1] = kStack; native.cpu.gpr[2] = kSda2; native.cpu.gpr[13] = kSda;
            native.cpu.gpr[3] = kOut; native.cpu.gpr[4] = kIn; native.cpu.gpr[5] = 0u; native.cpu.gpr[6] = kMatrix;
            native.cpu.run_steps(1);
            require(native.cpu.stopped() && native.cpu.stop().detail.find("count of 0") != std::string::npos,
                    "a count of 0 must stop the machine by name");
        }
        for (int kind = 0; kind < 2; ++kind)
            require(caught(argv[1], [kind](Cpu& cpu) { install_wrong_point(cpu, kind); }),
                    "negative control: a wrong point transform (" + std::to_string(kind) + ") was not told apart");
        for (int kind = 3; kind < 5; ++kind)
            require(caught(argv[1], [kind](Cpu& cpu) { install_wrong_sqrt(cpu, kind); }, true),
                    "negative control: a wrong square-root owner (" + std::to_string(kind) + ") was not told apart");
        std::printf("PASS rw vector contract: %u original-vs-native cases (%u RwV3dNormalize error-path cases skipped)\n", cases,
                    g_skipped);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL rw vector contract: %s\n", error.what());
        return 1;
    }
}
