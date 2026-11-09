// liveness-roots: reference-interpreter-only
// Original-DOL contract for the native RenderWare matrix routines (gekko/native_rw_matrix.h): RwMatrixInvert 0x80480100, RwMatrixScale 0x804803E8, RwMatrixTranslate 0x804805EC and
// RwMatrixTransform 0x8048077C (operations replace / pre-concatenate / post-concatenate; invalid operations are declined and not
// compared); RwMatrixUpdate 0x8047FBF0 and RwMatrixOptimize 0x8047F9E0 (near-identity / rotation / ordinary matrices with random
// tolerances, the default tolerances of the engine included); RwMatrixRotateOneMinusCosineSine 0x8047FD24 (axis in r4, f1 = 1 - cos, f2 = sin, the second slot of f14..f31 is
// compared too, since the original saves f31 with a psq pair). The three pad words of a Transform product hold stale stack bytes in the original and are masked out.
//
// Reference interpreter, pinned DOL. Each case builds one memory state (a source matrix, an output matrix that may overlap it,
// the engine's matrix-module globals) and runs the ORIGINAL body and the owner from byte-identical memory, then compares every
// byte of a 4 KiB window, r3, r1, r2, r13, r14..r31, LR and f14..f31. The volatile registers (f0..f13, r4..r12, CTR, CR) are not
// compared: nothing reads them after the call (docs/NATIVE_MATH_METHOD.md); f31, which the original spills and restores, is compared.
// Matrix words are random bit patterns drawn from every float class (normals of both signs, zeros, subnormals, values near
// the underflow boundary, infinities, quiet and signalling NaNs, extremes) mixed with ordinary rotation / scale matrices so that
// the determinant is usually finite and non-zero; the flags cover identity (with and without the enabled optimisation),
// orthonormal, normal and orthogonal; both FPSCR[NI] settings; dst separate, dst = src and dst overlapping src.
//
// Negative controls: owners that skip the reciprocal's zero test, use an unfused cofactor or transpose without the translation
// are told apart.
//
// usage: rw_matrix_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_rw_matrix.h"

#include <cmath>
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
namespace rm = gekko::rw_matrix;

constexpr u32 kSda = 0x805EC500u, kSda2 = 0x805FA780u;
constexpr u32 kStack = 0x817E0000u, kFinish = 0x817FF000u;
constexpr u32 kWindow = 0x81302000u, kWindowBytes = 0x1000u;
constexpr u32 kSrc = 0x81302100u, kDst = 0x81302400u;
constexpr u32 kEngine = 0x805E3C08u, kModuleOffset = 0x300u;

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

u32 float_bits(float f) {
    u32 w;
    std::memcpy(&w, &f, 4);
    return w;
}

u32 random_float(Rng& rng) {
    const u32 sign = rng.below(2) ? 0x80000000u : 0u;
    switch (rng.below(16)) {
        case 0: return sign;
        case 1: return sign | (rng.next() & 0x007FFFFFu);
        case 2: return sign | 0x7F800000u;
        case 3: return sign | 0x7F800000u | (rng.next() & 0x007FFFFFu) | 1u;
        case 4: return sign | 0x7FC00000u | (rng.next() & 0x003FFFFFu);
        case 5: return sign | 0x00800000u | (rng.next() & 0x000FFFFFu);
        case 6: return sign | 0x7F7FFFFFu;
        case 7: return sign | ((0x3F000000u - 0x0C000000u) + (rng.next() & 0x00FFFFFFu));
        default: return float_bits(static_cast<float>(static_cast<int>(rng.below(2001)) - 1000) / 64.0f);
    }
}

// An ordinary matrix: a rotation-like block (cosines / sines of a random angle with a random scale) and a translation.
void ordinary(Rng& rng, u32 out[12]) {
    const float a = static_cast<float>(rng.below(6283)) / 1000.0f, b = static_cast<float>(rng.below(6283)) / 1000.0f;
    const float s = 0.25f + static_cast<float>(rng.below(400)) / 100.0f;
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
    const float m[9] = {ca * cb * s, sa * s, -ca * sb * s, -sa * cb * s, ca * s, sa * sb * s, sb * s, 0.0f, cb * s};
    for (int i = 0; i < 9; ++i) out[i] = float_bits(m[i]);
    for (int i = 9; i < 12; ++i) out[i] = float_bits(static_cast<float>(static_cast<int>(rng.below(2001)) - 1000) / 16.0f);
}

struct Snapshot {
    u32 r3, r1, r2, r13, lr;
    u32 saved[18];
    u64 f[18], f_second[18];
    std::vector<u8> window;
};

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};

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
            rm::install(cpu);
            if (wrong) wrong(cpu);
        }
    }

    bool has_f = false;
    u64 f1_value = 0, f2_value = 0;

    void w(u32 address, u32 value) { memory.write32(address, value); }

    void call(u32 entry, u32 dst, u32 src, u32 op, bool ni) {
        cpu.reset();
        cpu.set_msr(cpu.msr() | 0x2000u);
        cpu.spr[SPR_GQR0] = 0u;
        cpu.fpscr = ni ? kFpscrNI : 0u;
        cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = kFinish;
        for (unsigned i = 0; i < 32; ++i) cpu.gpr[i] = 0x1000u + i * 0x111u;
        cpu.gpr[1] = kStack; cpu.gpr[2] = kSda2; cpu.gpr[13] = kSda;
        cpu.gpr[3] = dst; cpu.gpr[4] = src; cpu.gpr[5] = op;
        for (unsigned i = 0; i < 32; ++i) {
            const u64 value = 0x4000000000000000ull + i;
            std::memcpy(&cpu.fpr[i].bits[0], &value, sizeof(value));
            std::memcpy(&cpu.fpr[i].bits[1], &value, sizeof(value));
        }
        if (has_f) {
            cpu.fpr[1].bits[0] = cpu.fpr[1].bits[1] = f1_value;
            cpu.fpr[2].bits[0] = cpu.fpr[2].bits[1] = f2_value;
        }
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
        s.window.resize(kWindowBytes);
        for (u32 i = 0; i < kWindowBytes; ++i) s.window[i] = memory.peek8(kWindow + i);
        return s;
    }
};

std::string diff(const Snapshot& a, const Snapshot& b, u32 skip0, u32 skip1, u32 skip2) {
    char text[140];
    if (a.r3 != b.r3) { std::snprintf(text, sizeof text, "r3 original=%08X native=%08X", a.r3, b.r3); return text; }
    if (a.r1 != b.r1 || a.r2 != b.r2 || a.r13 != b.r13 || a.lr != b.lr) return "r1 / r2 / r13 / lr differ";
    if (std::memcmp(a.saved, b.saved, sizeof a.saved) != 0) return "a callee-saved register differs";
    if (std::memcmp(a.f, b.f, sizeof a.f) != 0) return "a callee-saved FPR differs";
    if (std::memcmp(a.f_second, b.f_second, sizeof a.f_second) != 0) return "a callee-saved FPR second slot differs";
    for (u32 i = 0; i < kWindowBytes; i += 4) {
        u32 x, y;
        std::memcpy(&x, &a.window[i], 4);
        std::memcpy(&y, &b.window[i], 4);
        if (x != y && kWindow + i != skip0 && kWindow + i != skip1 && kWindow + i != skip2) { std::snprintf(text, sizeof text, "window word 0x%08X differs (original %08X native %08X)", kWindow + i, x, y); return text; }
    }
    return {};
}

unsigned g_cases = 0;
u32 g_only_entry = 0;  // a negative control only runs the cases of the entry its wrong owner replaces
const std::function<void(Cpu&)>* g_wrong = nullptr;

struct Case {
    u32 entry, dst, src, op, flags, optimisations, seed;
    bool ni, ordinary_matrix;
    u64 f1 = 0, f2 = 0;
    bool uses_f = false;
    bool optimize = false;
};

void fill(Machine& m, const Case& k) {
    for (u32 a = kWindow; a < kWindow + kWindowBytes; a += 4) m.w(a, 0x5A5A5A5Au ^ (a * 40503u));
    Rng rng(k.seed);
    u32 words[12];
    if (k.ordinary_matrix) ordinary(rng, words);
    else
        for (u32& word : words) word = random_float(rng);
    const u32 rows[4] = {0x00u, 0x10u, 0x20u, 0x30u};
    const u32 matrix = k.entry == rm::kInvert ? k.src : k.dst;  // Invert reads src; the others modify their first operand
    for (u32 r = 0; r < 4; ++r)
        for (u32 j = 0; j < 3; ++j) m.w(matrix + rows[r] + 4 * j, words[3 * r + j]);
    m.w(matrix + 0x0Cu, k.flags);
    m.w(matrix + 0x1Cu, rng.next());
    m.w(matrix + 0x2Cu, rng.next());
    m.w(matrix + 0x3Cu, rng.next());
    // the engine instance and the matrix module's globals (offset 4 holds the enabled optimisations)
    m.w(kSda + 0x615Cu, kEngine);
    m.w(kSda + 0x6108u, kModuleOffset);
    m.w(kEngine + kModuleOffset + 4u, k.optimisations);
    // the second operand: a scale / translation vector or a transform matrix at kDst
    u32 other[12];
    if (k.ordinary_matrix) ordinary(rng, other);
    else
        for (u32& word : other) word = random_float(rng);
    if (matrix != kDst) {
        for (u32 r = 0; r < 4; ++r)
            for (u32 j = 0; j < 3; ++j) m.w(kDst + rows[r] + 4 * j, other[3 * r + j]);
        m.w(kDst + 0x0Cu, k.flags ^ 0x20003u);
        m.w(kDst + 0x1Cu, rng.next());
        m.w(kDst + 0x2Cu, rng.next());
        m.w(kDst + 0x3Cu, rng.next());
    }
}

constexpr u32 kTolerances = kDst + 0x80u;

// RwMatrixOptimize inputs: an identity, a rotation or an ordinary matrix perturbed by a relative error of 0 .. 1e-3, and tolerances
// from the useful range plus the degenerate ones (0, negative, infinity, NaN); the engine's default tolerances are the same triple.
void fill_optimize(Machine& m, const Case& k) {
    Rng rng(k.seed + 555u);
    u32 words[12];
    const u32 type = rng.below(3);
    if (type == 0) {
        const u32 identity[12] = {0x3F800000u, 0, 0, 0, 0x3F800000u, 0, 0, 0, 0x3F800000u, 0, 0, 0};
        std::memcpy(words, identity, sizeof words);
    } else if (type == 1) {
        const float a = static_cast<float>(rng.below(6283)) / 1000.0f, b = static_cast<float>(rng.below(6283)) / 1000.0f;
        const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
        const float r[9] = {ca * cb, sa, -ca * sb, -sa * cb, ca, sa * sb, sb, 0.0f, cb};
        for (int i = 0; i < 9; ++i) words[i] = float_bits(r[i]);
        for (int i = 9; i < 12; ++i) words[i] = float_bits(static_cast<float>(static_cast<int>(rng.below(2001)) - 1000) / 16.0f);
    } else {
        ordinary(rng, words);
    }
    const float errors[4] = {0.0f, 1e-7f, 1e-5f, 1e-3f};
    const float error = errors[rng.below(4)];
    for (u32& w : words) {
        float f;
        std::memcpy(&f, &w, 4);
        f += error * (static_cast<float>(rng.below(2001)) - 1000.0f) / 1000.0f;
        w = float_bits(f);
    }
    if (rng.below(12) == 0) words[rng.below(12)] = random_float(rng);
    const u32 rows[4] = {0x00u, 0x10u, 0x20u, 0x30u};
    for (u32 r = 0; r < 4; ++r)
        for (u32 j = 0; j < 3; ++j) m.w(k.dst + rows[r] + 4 * j, words[3 * r + j]);
    const float tolerances[] = {0.0f, -1.0f, 1e-6f, 1e-5f, 1e-4f, 1e-3f, 0.01f, 0.1f, 1.0f, HUGE_VALF, NAN};
    u32 triple[3];
    for (u32& t : triple) t = float_bits(tolerances[rng.below(sizeof tolerances / sizeof tolerances[0])]);
    for (u32 i = 0; i < 3; ++i) {
        m.w(kTolerances + 4 * i, triple[i]);
        m.w(kEngine + kModuleOffset + 0xCu + 4 * i, triple[i]);
    }
}

void compare(const char* path, const char* what, const Case& k) {
    if (g_only_entry != 0u && k.entry != g_only_entry) return;
    Machine original(path, false), native(path, true, g_wrong ? *g_wrong : std::function<void(Cpu&)>{});
    fill(original, k);
    fill(native, k);
    if (k.optimize) {
        fill_optimize(original, k);
        fill_optimize(native, k);
    }
    if (k.uses_f) {
        original.has_f = native.has_f = true;
        original.f1_value = native.f1_value = k.f1;
        original.f2_value = native.f2_value = k.f2;
    }
    original.call(k.entry, k.dst, k.src, k.op, k.ni);
    native.call(k.entry, k.dst, k.src, k.op, k.ni);
    const bool masked = (k.entry == rm::kTransform && k.op != 0u) || k.entry == rm::kRotate;
    const std::string d = diff(original.snapshot(), native.snapshot(), masked ? k.dst + 0x1Cu : 0u, masked ? k.dst + 0x2Cu : 0u,
                               masked ? k.dst + 0x3Cu : 0u);
    if (!d.empty()) {
        char text[260];
        std::snprintf(text, sizeof text, "%s (seed %u, ni %d, flags %08X, opt %08X, dst %08X src %08X, %s matrix): %s", what,
                      k.seed, k.ni ? 1 : 0, k.flags, k.optimisations, k.dst, k.src, k.ordinary_matrix ? "ordinary" : "random",
                      d.c_str());
        throw std::runtime_error(text);
    }
    ++g_cases;
}

void run_all(const char* path) {
    g_cases = 0;
    u32 seed = 1;
    const u32 flag_values[] = {0x0u, 0x1u, 0x2u, 0x3u, 0x20000u, 0x20003u, 0x7u, 0xFFFFFFFFu};
    const u32 optimisation_values[] = {0u, 0x20000u, 0xFFFFFFFFu};
    for (const bool ni : {false, true})
        for (const u32 dst : {kDst, kSrc, kSrc + 4u, kSrc + 0x10u, kSrc + 0x30u})
            for (const u32 flags : flag_values)
                for (const u32 opt : optimisation_values)
                    for (u32 repeat = 0; repeat < 3; ++repeat)
                        compare(path, "matrix invert", Case{rm::kInvert, dst, kSrc, 0u, flags, opt, seed++, ni, repeat != 0});
    // Scale / Translate / Transform: matrix at kSrc; the operand at kDst, inside the matrix, or the matrix itself
    // Rotate: a random axis (any float class) and (1 - cos, sin) as widened singles or doubles, ordinary or extreme
    for (const bool ni : {false, true}) {
        Rng rng(seed + 9000u);
        for (const u32 operand : {kDst, kSrc + 0x10u})
            for (const u32 op : {0u, 1u, 2u})
                for (const u32 flags : {0x0u, 0x3u, 0x20003u, 0xFFFFFFFFu})
                    for (u32 repeat = 0; repeat < 10; ++repeat) {
                        const auto angle = [&]() {
                            const u64 pick = rng.below(4);
                            if (pick == 0) return static_cast<u64>(rng.next()) << 32 | rng.next();
                            return single_to_double_bits(random_float(rng));
                        };
                        Case k{rm::kRotate, kSrc, operand, op, flags, 0x20000u, seed++, ni, repeat % 3 != 0};
                        k.f1 = angle();
                        k.f2 = angle();
                        k.uses_f = true;
                        compare(path, "matrix rotate", k);
                    }
    }
    // Update / Optimize
    for (const bool ni : {false, true}) {
        for (const u32 flags : {0x0u, 0x1u, 0x3u, 0x20003u, 0xFFFFFFFFu, 0x20000u, 0x12345678u})
            compare(path, "matrix update", Case{rm::kUpdate, kSrc, 0u, 0u, flags, 0u, seed++, ni, false});
        for (const u32 tolerances : {kTolerances, 0u})
            for (const u32 flags : {0x0u, 0x20003u, 0xFFFFFFFFu})
                for (u32 repeat = 0; repeat < 220; ++repeat) {
                    Case k{rm::kOptimize, kSrc, tolerances, 0u, flags, 0u, seed++, ni, false};
                    k.optimize = true;
                    compare(path, "matrix optimize", k);
                }
    }
    const u32 entries[] = {rm::kScale, rm::kTranslate, rm::kTransform};
    for (const u32 entry : entries)
        for (const bool ni : {false, true})
            for (const u32 operand : {kDst, kSrc, kSrc + 4u, kSrc + 0x10u, kSrc + 0x30u})
                for (const u32 op : {0u, 1u, 2u})
                    for (const u32 flags : flag_values)
                        for (const u32 opt : {0u, 0x20000u})
                            for (u32 repeat = 0; repeat < 3; ++repeat)
                                compare(path, entry == rm::kScale ? "matrix scale" : entry == rm::kTranslate ? "matrix translate" : "matrix transform",
                                        Case{entry, kSrc, operand, op, flags, opt, seed++, ni, repeat != 0});
}

bool caught(const char* path, const std::function<void(Cpu&)>& wrong, u32 only_entry) {
    g_wrong = &wrong;
    g_only_entry = only_entry;
    bool found = false;
    try {
        run_all(path);
    } catch (const std::exception&) {
        found = true;
    }
    g_wrong = nullptr;
    g_only_entry = 0u;
    return found;
}

// A wrong owner: kind 0 = the identity branch ignores the enabled optimisations; kind 1 = the orthonormal branch leaves the
// translation untouched.
constexpr u32 kControlEntries[8] = {rm::kInvert, rm::kInvert, rm::kScale, rm::kTranslate, rm::kTransform, rm::kRotate, rm::kOptimize, rm::kUpdate};

void install_wrong_other(Cpu& cpu, int kind) {
    // 2: Scale leaves the flags alone; 3: Translate adds the translation without the matrix rows; 4: Transform swaps its operations
    if (kind == 2)
        cpu.install_hle(rm::kScale, [](Cpu& c) {
            const u32 matrix = c.gpr[3];
            const u32 flags = c.mem_read32(matrix + 0xCu);
            const bool ok = rm::serve_scale(c);
            if (ok) c.mem_write32(matrix + 0xCu, flags);
            return ok;
        }, "wrong matrix scale");
    if (kind == 3)
        cpu.install_hle(rm::kTranslate, [](Cpu& c) {
            const u32 matrix = c.gpr[3];
            const u32 pos = c.mem_read32(matrix + 0x30u);
            const bool ok = rm::serve_translate(c);
            if (ok && c.gpr[5] == 1u) c.mem_write32(matrix + 0x30u, pos);
            return ok;
        }, "wrong matrix translate");
    if (kind == 6)
        cpu.install_hle(rm::kOptimize, [](Cpu& c) {
            const u32 matrix = c.gpr[3];
            const bool ok = rm::serve_optimize(c);
            if (ok) c.mem_write32(matrix + 0xCu, c.mem_read32(matrix + 0xCu) & ~0x20000u);  // never marks the identity
            return ok;
        }, "wrong matrix optimize");
    if (kind == 7)
        cpu.install_hle(rm::kUpdate, [](Cpu& c) {
            c.mem_write32(c.gpr[3] + 0xCu, c.mem_read32(c.gpr[3] + 0xCu) & 0xFFFEFFFCu);
            return true;
        }, "wrong matrix update");
    if (kind == 5)
        cpu.install_hle(rm::kRotate, [](Cpu& c) {
            c.fpr[2].bits[0] ^= 0x8000000000000000ull;  // the sine's sign
            return rm::serve_rotate(c);
        }, "wrong matrix rotate");
    if (kind == 4)
        cpu.install_hle(rm::kTransform, [](Cpu& c) {
            if (c.gpr[5] == 1u || c.gpr[5] == 2u) c.gpr[5] = 3u - c.gpr[5];
            return rm::serve_transform(c);
        }, "wrong matrix transform");
}

void install_wrong(Cpu& cpu, int kind) {
    if (kind >= 2) return install_wrong_other(cpu, kind);
    cpu.install_hle(rm::kInvert, [kind](Cpu& c) {
        const u32 dst = c.gpr[3], src = c.gpr[4];
        const u32 flags = c.mem_read32(src + 0xCu);
        if (kind == 0 && (flags & 0x20000u) != 0u) {
            for (u32 i = 0; i < 16; ++i) c.mem_write32(dst + 4 * i, c.mem_read32(src + 4 * i));
            return true;
        }
        if (kind == 1 && (flags & 3u) == 3u) {
            for (u32 r = 0; r < 3; ++r)
                for (u32 j = 0; j < 3; ++j) c.mem_write32(dst + 0x10u * r + 4 * j, c.mem_read32(src + 0x10u * j + 4 * r));
            c.mem_write32(dst + 0xCu, 3u);
            return true;
        }
        return rm::serve_invert(c);
    }, "wrong matrix invert");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rw_matrix_contract_tests main.dol");
        run_all(argv[1]);
        const unsigned cases = g_cases;
        for (int kind = 0; kind < 8; ++kind)
            require(caught(argv[1], [kind](Cpu& cpu) { install_wrong(cpu, kind); }, kControlEntries[kind]),
                    "negative control: a wrong matrix invert (" + std::to_string(kind) + ") was not told apart");
        std::printf("PASS rw matrix contract: %u original-vs-native cases\n", cases);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL rw matrix contract: %s\n", error.what());
        return 1;
    }
}
