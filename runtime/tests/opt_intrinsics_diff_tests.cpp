// Differential test for the plain-float routes of the host intrinsics (optimisation round 2, docs/agents/opt-intrinsics.md).
// The old implementation (the untouched ppc::fps_* / shadow::math::detail sequences, still in the tree as the fallback of
// every fast route) against the new one, bit for bit, on random, edge, NaN, infinity and denormal inputs, with FPSCR[NI]
// both ways and sticky bits already set:
//   1. the primitives: whenever fast::Check accepts a computation, the plain float result equals the ppc::fps_* result and
//      the exact route changes no FPSCR bit (this is what makes every composition below exact);
//   2. fast::LazyCompare against the fp_compare sequence it replaces (cr0, FPCC, sticky exception bits, the EQ answers);
//   3. gekko::chara_colli_geometry::distance_squared against distance_squared_exact: result bits, CR, FPSCR;
//   4. RwMatrixMultiply: the single-precision wide body (execute) against the ordered body (run), all registers and memory;
//   5. shadow::math::compose_affine_matrix (disjoint spans: the row-per-vector route) against the scalar contract, both
//      subnormal modes, every output word;
//   6. DynamicCastIntrinsic::cast with its front cache against a fresh instance (no cache, no memo) after random changes of
//      the vtable words and the objects' vtable pointers (RTTI structures are immutable, as in the memo's own rule);
//   7. math_affine serve_point / translate_by / scale_columns and vec_math serve_* against their *_exact bodies: every
//      register, CR, FPSCR and every byte of an arena, including outputs aliasing inputs.
// Negative controls: a deliberately wrong "fast" primitive, a compare that forgets the last write and a one-ulp perturbed
// result must each be caught. usage: opt_intrinsics_diff_tests [iterations-scale]
#include "gekko/chara_colli_geometry.h"
#include "gekko/cpu.h"
#include "gekko/dynamic_cast_intrinsic.h"
#include "gekko/fast_single.h"
#include "gekko/guest_memory.h"
#include "gekko/matrix_multiply_intrinsic.h"
#include "gekko/native_math_affine.h"
#include "gekko/native_vec_math.h"
#include "gekko/ppc_ops.h"
#include "math/floating_point.h"
#include "math/matrix.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
constexpr u32 kArena = 0x80700000u;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }
void require(bool condition, const std::string& message) {
    if (!condition) fail(message);
}

u64 g_state = 0x9E3779B97F4A7C15ull;
u64 rnd64() {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 7;
    g_state ^= g_state << 17;
    return g_state;
}
u32 rnd() { return static_cast<u32>(rnd64() >> 16); }

constexpr u32 kSign = 0x80000000u;
constexpr u32 kSpecial[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007FFFFFu, kSign | 0x007FFFFFu, 0x00800000u,
                            0x80800000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7FC12345u, 0xFFD00001u,
                            0x7F800001u, 0xFF800001u, 0x3F800000u, 0xBF800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x34000000u,
                            0x4B800000u, 0x0D800000u, kSign | 0x0D800000u, 0x0D7FFFFFu, 0x03800000u, 0x00FFFFFFu, 0x7F000000u};

// A float word: mostly ordinary values of everyday size, with the whole range, the boundaries of the ordinary range
// (2^-100, 2^-126, 2^127) and the special classes mixed in. `ordinary_bias` out of 16 words are plain ordinary.
u32 float_word(u32 ordinary_bias) {
    const u32 sign = (rnd() & 1u) << 31;
    if (rnd() % 16u < ordinary_bias) {
        const int exponent = 127 + static_cast<int>(rnd() % 61u) - 30;  // 2^-30 .. 2^30
        const u32 mantissa = (rnd() & 3u) == 0u ? (rnd() & 0x7FF000u) : (rnd() & 0x7FFFFFu);
        return sign | (static_cast<u32>(exponent) << 23) | mantissa;
    }
    switch (rnd() % 8u) {
    case 0: return kSpecial[rnd() % (sizeof(kSpecial) / sizeof(kSpecial[0]))];
    case 1: return rnd();
    case 2: return sign | ((rnd() % 255u + 1u) << 23) | (rnd() & 0x7FFFFFu);  // any finite normal
    case 3: return sign | (rnd() & 0x007FFFFFu);                                // zero / denormal
    case 4: return sign | ((90u + rnd() % 30u) << 23) | (rnd() & 0x7FFFFFu);    // near 2^-37 .. (not tiny)
    case 5: return sign | ((20u + rnd() % 20u) << 23) | (rnd() & 0x7FFFFFu);    // 2^-107 .. 2^-88: the guard band
    case 6: return sign | ((235u + rnd() % 20u) << 23) | (rnd() & 0x7FFFFFu);   // huge: results overflow
    default: return sign | (0x7F800000u | (rnd() & 0x7FFFFFu));                 // inf / NaN
    }
}

// A fused-multiply-add triple that sits on the double-rounding trap: a * c is exactly a single midpoint (a small odd
// integer times an even integer) and b moves the exact value a hair off it - far below the double's precision.
struct Triple {
    u32 a, c, b;
};
Triple trap_triple() {
    for (;;) {
        const u32 small = 3u + 2u * (rnd() % 30u);           // odd 3 .. 61
        const u32 big = (rnd() & 0xFFFFFFu) | 0x800000u;     // 24-bit integer
        const u64 p = static_cast<u64>(small) * big;
        int bits = 0;
        while ((p >> bits) != 0u) ++bits;
        if (bits <= 24) continue;
        const u64 ulp = u64{1} << (bits - 24);
        if ((p & (ulp - 1u)) != ulp / 2u) continue;  // not a tie
        const int shift = static_cast<int>(rnd() % 40u) - 20;
        const u32 sign_a = (rnd() & 1u) << 31, sign_c = (rnd() & 1u) << 31;
        const float a = std::ldexp(static_cast<float>(small), shift) * (sign_a ? -1.0f : 1.0f);
        const float c = static_cast<float>(big) * (sign_c ? -1.0f : 1.0f);
        const double product = static_cast<double>(a) * static_cast<double>(c);
        // b: a power of two far below the product's last bit, either sign, or exactly zero.
        const int e = std::ilogb(product) - 40 - static_cast<int>(rnd() % 8u);
        float b = 0.0f;
        if (rnd() % 4u != 0u && e > -120) b = std::ldexp((rnd() & 1u) ? 1.0f : -1.0f, e);
        return {float_to_bits(a), float_to_bits(c), float_to_bits(b)};
    }
}

// ---- primitives ----------------------------------------------------------------------------------------------------

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    Machine() { cpu.reset(); }
};

u32 random_fpscr() {
    u32 value = (rnd() & 1u) ? kFpscrNI : 0u;
    if (rnd() & 1u) value |= rnd() & 0xFFF80000u;
    return value & ~kFpscrFPCC;
}

struct Counts {
    u64 total = 0, accepted = 0;
};

// The fast primitives under test. `which` selects the operation.
enum Op { kAdd, kSub, kMul, kDiv, kMAdd, kMSub, kNMSub, kNMAdd, kOps };
const char* const kOpName[kOps] = {"add", "sub", "mul", "div", "madd", "msub", "nmsub", "nmadd"};

// Returns the plain float result (the fast side) after feeding the Check.
float fast_op(Op op, fast::Check& k, u32 wa, u32 wb, u32 wc) {
    const float a = k.in(wa), b = k.in(wb), c = k.in(wc);
    switch (op) {
    case kAdd: return k.out(a + b);
    case kSub: return k.out(a - b);
    case kMul: return k.out(a * c);
    case kDiv: return k.out(a / b);
    case kMAdd: return k.out(fast::fma(a, c, b));
    case kMSub: return k.out(fast::fma(a, c, -b));
    case kNMSub: return k.out(-fast::fma(a, c, -b));
    default: return k.out(-fast::fma(a, c, b));
    }
}
u64 exact_op(Op op, Cpu& cpu, u32 wa, u32 wb, u32 wc) {
    const u64 a = single_to_double_bits(wa), b = single_to_double_bits(wb), c = single_to_double_bits(wc);
    switch (op) {
    case kAdd: return ppc::fps_add(cpu, a, b);
    case kSub: return ppc::fps_sub(cpu, a, b);
    case kMul: return ppc::fps_mul(cpu, a, c);
    case kDiv: return ppc::fps_div(cpu, a, b);
    case kMAdd: return ppc::fps_madd(cpu, a, c, b);
    case kMSub: return ppc::fps_msub(cpu, a, c, b);
    case kNMSub: return ppc::fps_nmsub(cpu, a, c, b);
    default: return ppc::fps_nmadd(cpu, a, c, b);
    }
}

// mutate: when non-zero the fast result is perturbed (the negative control).
Counts run_primitives(u64 iterations, u32 mutate) {
    Counts counts[kOps];
    Machine m;
    for (u64 i = 0; i < iterations; ++i) {
        const Op op = static_cast<Op>(rnd() % kOps);
        u32 wa, wb, wc;
        const u32 mode = rnd() % 8u;
        if ((op == kMAdd || op == kMSub || op == kNMSub || op == kNMAdd) && mode == 0u) {
            const Triple t = trap_triple();
            wa = t.a, wc = t.c, wb = t.b;
            if (op == kMSub || op == kNMSub) wb ^= 0x80000000u;  // keep the addend's effective sign
        } else {
            const u32 bias = mode < 5u ? 16u : 12u;
            wa = float_word(bias), wb = float_word(bias), wc = float_word(bias);
        }
        m.cpu.fpscr = random_fpscr();
        const u32 before = m.cpu.fpscr;
        fast::Check k;
        float fast_result = fast_op(op, k, wa, wb, wc);
        ++counts[op].total;
        if (!k.ok()) continue;
        ++counts[op].accepted;
        if (mutate) fast_result = bits_to_float(float_to_bits(fast_result) ^ 1u);
        const u64 exact = exact_op(op, m.cpu, wa, wb, wc);
        require(m.cpu.fpscr == before, std::string("the exact ") + kOpName[op] + " changed FPSCR on an accepted input");
        if (fast::fpr_of(fast_result) != exact) {
            char text[200];
            std::snprintf(text, sizeof text, "%s: a %08X b %08X c %08X fast %08X exact %016llX", kOpName[op], wa, wb, wc,
                          float_to_bits(fast_result), static_cast<unsigned long long>(exact));
            fail(text);
        }
    }
    Counts sum;
    for (const Counts& c : counts) {
        sum.total += c.total;
        sum.accepted += c.accepted;
        require(c.accepted * 4u >= c.total || iterations < 1000u, "too few accepted inputs: the test would not cover the fast route");
    }
    return sum;
}

// ---- LazyCompare -----------------------------------------------------------------------------------------------------

void run_compare(u64 iterations, bool drop_last_write) {
    Machine fast_machine, exact_machine;
    for (u64 i = 0; i < iterations; ++i) {
        Cpu& f = fast_machine.cpu;
        Cpu& e = exact_machine.cpu;
        const u32 cr = rnd(), fpscr = random_fpscr();
        f.cr = e.cr = cr;
        f.fpscr = e.fpscr = fpscr;
        const u32 steps = 1u + rnd() % 7u;
        std::vector<bool> eq_fast, eq_exact;
        {
            fast::LazyCompare compare(f);
            for (u32 s = 0; s < steps; ++s) {
                const u32 a = float_word(10u), b = (rnd() & 1u) ? a : float_word(10u);
                const bool ordered = (rnd() & 1u) != 0u;
                eq_fast.push_back(compare.equal(a, b, ordered));
                ppc::fp_compare(e, 0u, single_to_double_bits(a), single_to_double_bits(b), ordered);
                eq_exact.push_back(ppc::cr_bit(e, 2u));
            }
            if (drop_last_write) f.cr = cr;  // never reached by the real code: the control loses the write below
        }
        if (drop_last_write) {
            // control: pretend the destructor's write did not happen
            f.cr = cr;
            f.fpscr = fpscr;
        }
        require(eq_fast == eq_exact, "the EQ answers differ");
        require(f.cr == e.cr, "CR differs after the compare sequence");
        require(f.fpscr == e.fpscr, "FPSCR differs after the compare sequence");
    }
}

// ---- distance_squared -------------------------------------------------------------------------------------------------

void fill_v3(chara_colli_geometry::V3& v, u32 bias) {
    v.x = float_word(bias), v.y = float_word(bias), v.z = float_word(bias);
}

u64 run_distance(u64 iterations, u32 mutate) {
    using namespace chara_colli_geometry;
    Machine mf, me;
    u64 fast_taken = 0;
    for (u64 i = 0; i < iterations; ++i) {
        V3 centre, a, b;
        const u32 mode = rnd() % 8u;
        const u32 bias = mode < 6u ? 16u : 12u;
        fill_v3(centre, bias), fill_v3(a, bias), fill_v3(b, bias);
        if (mode == 1u) b = a;  // degenerate segment
        if (mode == 2u) {       // the centre on / near the segment
            const float t = static_cast<float>(rnd() % 5u) * 0.25f;
            const float ax = bits_to_float(a.x), bx = bits_to_float(b.x);
            centre.x = float_to_bits(ax + (bx - ax) * t);
            centre.y = a.y, centre.z = a.z;
        }
        if (mode == 3u) centre = a;
        if (mode == 4u) centre = b;
        const u32 constant = (rnd() % 3u == 0u) ? float_word(12u) : ((rnd() & 1u) ? 0u : 0x80000000u);
        const u32 fpscr = random_fpscr(), cr = rnd();
        for (Machine* m : {&mf, &me}) {
            m->memory.write32(kArena + 0x20u, constant);
            m->cpu.gpr[2] = kArena + 0x20u - kSegmentConstant;
            m->cpu.cr = cr;
            m->cpu.fpscr = fpscr;
        }
        u64 fast_out;
        const bool took = fast::kAvailable && distance_squared_fast(mf.cpu, centre, a, b, constant, fast_out);
        // the same state, through the public entry: the fast route when it can, else the exact one
        mf.cpu.cr = cr;
        mf.cpu.fpscr = fpscr;
        u64 got = distance_squared(mf.cpu, centre, a, b);
        if (mutate) got ^= 1ull << 29;
        const u64 want = distance_squared_exact(me.cpu, centre, a, b);
        fast_taken += took;
        if (got != want || mf.cpu.cr != me.cpu.cr || mf.cpu.fpscr != me.cpu.fpscr) {
            char text[320];
            std::snprintf(text, sizeof text,
                          "distance_squared: centre %08X %08X %08X a %08X %08X %08X b %08X %08X %08X k %08X: got %016llX want "
                          "%016llX cr %08X/%08X fpscr %08X/%08X",
                          centre.x, centre.y, centre.z, a.x, a.y, a.z, b.x, b.y, b.z, constant,
                          static_cast<unsigned long long>(got), static_cast<unsigned long long>(want), mf.cpu.cr, me.cpu.cr,
                          mf.cpu.fpscr, me.cpu.fpscr);
            fail(text);
        }
    }
    return fast_taken;
}

// ---- whole-routine comparisons over an arena ---------------------------------------------------------------------------

constexpr u32 kWords = 0x100u;

struct Pair {
    Machine fast_side, exact_side;
};

void sync(Pair& p, const std::vector<u32>& arena, u32 fpscr, u32 seed, u32 r3, u32 r4, u32 r5, u64 f1) {
    for (Machine* m : {&p.fast_side, &p.exact_side}) {
        Cpu& c = m->cpu;
        c.reset();
        for (u32 i = 0; i < kWords; ++i) m->memory.write32(kArena + 4u * i, arena[i]);
        u32 x = seed;
        for (u32 i = 0; i < 32u; ++i) {
            x = x * 1664525u + 1013904223u;
            c.gpr[i] = x;
            c.fpr[i].bits[0] = (u64{x} << 32) | (x * 2654435761u);
            c.fpr[i].bits[1] = (u64{x ^ 0xA5A5A5A5u} << 32) | x;
        }
        c.gpr[1] = 0x817E0000u;
        c.gpr[3] = r3, c.gpr[4] = r4, c.gpr[5] = r5;
        c.fpr[1].bits[0] = f1;
        c.fpscr = fpscr;
        c.cr = seed ^ 0x5A5A5A5Au;
    }
}

void compare_machines(Pair& p, const std::string& what, u32 mutate) {
    Cpu& f = p.fast_side.cpu;
    Cpu& e = p.exact_side.cpu;
    if (mutate) f.fpr[1].bits[0] ^= 1u;
    for (u32 i = 0; i < 32u; ++i)
        require(f.gpr[i] == e.gpr[i], what + ": r" + std::to_string(i) + " differs");
    require(f.cr == e.cr && f.lr == e.lr && f.ctr == e.ctr && f.xer == e.xer, what + ": CR / LR / CTR / XER differ");
    require(f.fpscr == e.fpscr, what + ": FPSCR differs");
    for (u32 i = 0; i < 32u; ++i)
        require(f.fpr[i].bits[0] == e.fpr[i].bits[0] && f.fpr[i].bits[1] == e.fpr[i].bits[1],
                what + ": f" + std::to_string(i) + " differs");
    for (u32 i = 0; i < kWords; ++i)
        require(p.fast_side.memory.read32(kArena + 4u * i) == p.exact_side.memory.read32(kArena + 4u * i),
                what + ": arena word " + std::to_string(i) + " differs");
}

std::vector<u32> make_arena(u32 bias) {
    std::vector<u32> arena(kWords);
    for (u32& w : arena) w = rnd();
    for (u32 i = 0x20u; i < 0x80u; ++i) arena[i] = float_word(bias);
    return arena;
}

struct AffineCase {
    void (*fast_serve)(Cpu&);
    void (*exact_serve)(Cpu&);
    const char* name;
    int kind;  // 0 point, 1 translate_by / scale_columns
};

u64 run_affine(u64 iterations, u32 mutate) {
    const AffineCase cases[] = {{math_affine::serve_point, math_affine::serve_point_exact, "point", 0},
                                {math_affine::serve_translate_by, math_affine::serve_translate_by_exact, "translate_by", 1},
                                {math_affine::serve_scale_columns, math_affine::serve_scale_columns_exact, "scale_columns", 1}};
    Pair p;
    u64 done = 0;
    const u32 m_at = kArena + 0x80u, v_at = kArena + 0xC0u, out_at = kArena + 0x100u;
    for (const AffineCase& cs : cases)
        for (u64 i = 0; i < iterations; ++i) {
            const u32 mode = static_cast<u32>(i % 6u);
            u32 r3, r4, r5;
            if (cs.kind == 0) {
                r3 = out_at, r4 = m_at, r5 = v_at;
                if (mode == 1u) r3 = m_at;
                else if (mode == 2u) r3 = v_at;
                else if (mode == 3u) r3 = m_at + 4u * (1u + rnd() % 8u);
                else if (mode == 4u) r5 = m_at + 4u * (rnd() % 10u);
                else if (mode == 5u) r3 = v_at + 4u * (1u + rnd() % 2u);
            } else {
                r3 = m_at, r4 = v_at, r5 = 0u;
                if (mode == 2u || mode == 4u) r4 = m_at + 4u * (rnd() % 10u);
                else if (mode == 3u || mode == 5u) r4 = m_at + 0x10u * (rnd() % 2u) + 4u * (rnd() % 3u);
            }
            const std::vector<u32> arena = make_arena((i % 5u) ? 16u : 12u);
            sync(p, arena, random_fpscr(), rnd(), r3, r4, r5, single_to_double_bits(float_word(14u)));
            cs.fast_serve(p.fast_side.cpu);
            cs.exact_serve(p.exact_side.cpu);
            compare_machines(p, cs.name, mutate && (i % 7u == 0u));
            ++done;
        }
    return done;
}

struct VecCase {
    void (*fast_serve)(Cpu&) noexcept;
    void (*exact_serve)(Cpu&) noexcept;
    const char* name;
    int kind;  // 0: (out, a, b), 1: (dest, src), 2: (a, b) -> f1, 3: (v) -> f1, 4: scale (out, v), f1
};

u64 run_vec(u64 iterations, u32 mutate) {
    const VecCase cases[] = {{vec_math::serve_sub, vec_math::serve_sub_exact, "sub", 0},
                             {vec_math::serve_add, vec_math::serve_add_exact, "add", 0},
                             {vec_math::serve_cross, vec_math::serve_cross_exact, "cross", 0},
                             {vec_math::serve_add_to, vec_math::serve_add_to_exact, "add_to", 1},
                             {vec_math::serve_sub_from, vec_math::serve_sub_from_exact, "sub_from", 1},
                             {vec_math::serve_dot, vec_math::serve_dot_exact, "dot", 2},
                             {vec_math::serve_length, vec_math::serve_length_exact, "length", 3},
                             {vec_math::serve_scale, vec_math::serve_scale_exact, "scale", 4}};
    Pair p;
    u64 done = 0;
    for (const VecCase& cs : cases)
        for (u64 i = 0; i < iterations; ++i) {
            const u32 a_at = kArena + 0x80u, b_at = kArena + 0xA0u, out_at = kArena + 0xC0u;
            u32 r3 = out_at, r4 = a_at, r5 = b_at;
            const u32 mode = static_cast<u32>(i % 6u);
            if (cs.kind == 0) {
                if (mode == 1u) r3 = a_at;
                else if (mode == 2u) r3 = b_at;
                else if (mode == 3u) r3 = a_at + 4u * (1u + rnd() % 2u);
                else if (mode == 4u) r5 = a_at + 4u * (rnd() % 4u);
                else if (mode == 5u) r3 = b_at + 4u * (1u + rnd() % 2u);
            } else if (cs.kind == 1 || cs.kind == 2) {
                r3 = a_at, r4 = b_at;
                if (mode == 1u) r4 = a_at;
                else if (mode == 2u) r4 = a_at + 4u * (1u + rnd() % 2u);
                else if (mode == 3u) r4 = a_at - 4u * (1u + rnd() % 2u);
            } else if (cs.kind == 3) {
                r3 = a_at;
            } else {
                r3 = out_at, r4 = a_at;
                if (mode == 1u) r3 = a_at;
                else if (mode == 2u) r3 = a_at + 4u * (1u + rnd() % 2u);
            }
            const std::vector<u32> arena = make_arena((i % 5u) ? 16u : 12u);
            u64 f1 = single_to_double_bits(float_word(15u));
            if (rnd() % 16u == 0u) f1 = rnd64();  // not a single at all
            sync(p, arena, random_fpscr(), rnd(), r3, r4, r5, f1);
            cs.fast_serve(p.fast_side.cpu);
            cs.exact_serve(p.exact_side.cpu);
            compare_machines(p, cs.name, mutate && (i % 7u == 0u));
            ++done;
        }
    return done;
}

// ---- DynamicCastIntrinsic front cache -----------------------------------------------------------------------------------

u64 run_dynamic_cast(u64 iterations, u32 mutate) {
    Machine m;
    Cpu& c = m.cpu;
    const u32 base = kArena + 0x2000u;
    // 8 objects (+0: vtable pointer), 8 vtables {rtti, top}, 4 rtti {name, entry list = 0}, 4 names, 4 `to` records {name}.
    const auto object_at = [&](u32 i) { return base + 0x10u * i; };
    const auto vtable_at = [&](u32 i) { return base + 0x100u + 0x10u * i; };
    const auto rtti_at = [&](u32 i) { return base + 0x200u + 0x10u * i; };
    const auto name_at = [&](u32 i) { return base + 0x300u + 0x20u * i; };
    const auto to_at = [&](u32 i) { return base + 0x400u + 0x10u * i; };
    const char names[3][8] = {"Alpha", "Beta", "Gamma"};
    const auto write_name = [&](u32 i, u32 which) {
        for (u32 k = 0; k < 8u; ++k) m.memory.write8(name_at(i) + k, static_cast<u8>(names[which][k]));
    };
    for (u32 i = 0; i < 8u; ++i) {
        m.memory.write32(object_at(i), vtable_at(i));
        m.memory.write32(vtable_at(i), rtti_at(i % 4u));
        m.memory.write32(vtable_at(i) + 4u, 0u);
    }
    for (u32 i = 0; i < 4u; ++i) {
        write_name(i, i % 3u);
        m.memory.write32(rtti_at(i), name_at(i));
        m.memory.write32(rtti_at(i) + 4u, 0u);
        m.memory.write32(to_at(i), name_at((i + 1u) % 4u));
    }
    DynamicCastIntrinsic persistent;
    u64 found = 0;
    for (u64 n = 0; n < iterations; ++n) {
        switch (rnd() % 8u) {
        case 0: m.memory.write32(vtable_at(rnd() % 8u), (rnd() % 5u == 0u) ? 0u : rtti_at(rnd() % 4u)); break;
        case 1: m.memory.write32(vtable_at(rnd() % 8u) + 4u, (rnd() % 3u) * 8u); break;
        // (the names and the `to` records stay: the memo, old and new, assumes RTTI structures are immutable and
        // revalidates only the vtable's two words)
        case 4: m.memory.write32(object_at(rnd() % 8u), vtable_at(rnd() % 8u)); break;
        default: break;
        }
        const u32 object = (rnd() % 16u == 0u) ? 0u : object_at(rnd() % 8u);
        const u32 to = (rnd() % 10u == 0u) ? 0u : to_at(rnd() % 4u);
        const u32 from = to_at(rnd() % 4u);
        DynamicCastIntrinsic fresh;
        u32 want = fresh.cast(c, object, 0u, to, from);
        u32 got = persistent.cast(c, object, 0u, to, from);
        if (mutate && n % 11u == 0u) got ^= 4u;
        require(got == want, "DynamicCastIntrinsic: the cached cast differs from a fresh walk");
        found += want != 0u;
    }
    return found;
}

// ---- shadow::math::compose_affine_matrix --------------------------------------------------------------------------------

// The scalar contract (matrix.h): y product, fused x, fused z, then the translation added.
void reference_compose(float* left, const float* right, shadow::math::SubnormalMode mode) {
    namespace d = shadow::math::detail;
    float out[12];
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 4; ++column) {
            float result = d::multiply(left[row * 4 + 1], right[4 + column], mode);
            result = d::fused(left[row * 4], right[column], result, mode);
            result = d::fused(left[row * 4 + 2], right[8 + column], result, mode);
            if (column == 3) result = d::add(left[row * 4 + 3], result, mode);
            out[row * 4 + column] = result;
        }
    std::memcpy(left, out, sizeof(out));
}

u64 run_compose(u64 iterations, u32 mutate) {
    u64 clean = 0;
    for (u64 i = 0; i < iterations; ++i) {
        u32 words[24];
        for (u32& w : words) w = float_word(16u);
        const u32 mode_pick = static_cast<u32>(rnd() % 4u);
        if (mode_pick == 0u) {
            const u32 specials = 1u + rnd() % 2u;
            for (u32 k = 0; k < specials; ++k) words[rnd() % 24u] = float_word(0u);
        } else if (mode_pick == 1u) {
            // clean values near the flush boundary: products and sums that land around 2^-126
            for (u32 k = 0; k < 24u; ++k)
                if (rnd() % 3u == 0u) words[k] = (rnd() & 0x80000000u) | ((rnd() % 8u + 1u) << 23) | (rnd() & 0x7FFFFFu);
        } else {
            ++clean;
        }
        const auto mode = (rnd() & 1u) ? shadow::math::SubnormalMode::Flush : shadow::math::SubnormalMode::Preserve;
        float expected[24], actual[24];
        for (u32 k = 0; k < 24u; ++k) expected[k] = actual[k] = bits_to_float(words[k]);
        reference_compose(expected, expected + 12, mode);
        shadow::math::compose_affine_matrix(actual, actual + 12, mode);
        if (mutate && i % 5u == 0u) actual[3] = bits_to_float(float_to_bits(actual[3]) ^ 1u);
        for (u32 k = 0; k < 12u; ++k)
            if (float_to_bits(expected[k]) != float_to_bits(actual[k])) {
                char text[200];
                std::snprintf(text, sizeof text, "compose_affine_matrix (%s): element %u expected %08X got %08X",
                              mode == shadow::math::SubnormalMode::Flush ? "flush" : "preserve", k,
                              float_to_bits(expected[k]), float_to_bits(actual[k]));
                fail(text);
            }
    }
    return clean;
}

// ---- RwMatrixMultiply: execute() (wide, else ordered) against run() (the ordered body) -------------------------------------

u32 matrix_word(u32 bias, u32 index) {
    if ((index & 3u) == 3u) return rnd();  // the flags / padding word
    return float_word(bias);
}

u64 run_matrix_multiply(u64 iterations, u32 mutate, u64* wide_out) {
    Pair p;
    MatrixMultiplyIntrinsic wide_side, ordered_side;
    const u32 a_at = kArena + 0x80u, b_at = kArena + 0xC0u, out_at = kArena + 0x100u, sp_top = kArena + 0x1B0u;
    u64 calls = 0;
    for (u64 i = 0; i < iterations; ++i) {
        std::vector<u32> arena = make_arena(16u);
        const u32 bias = (i % 4u) ? 16u : 14u;
        for (u32 k = 0; k < 16u; ++k) {
            arena[(0x80u + 4u * k) / 4u] = matrix_word(bias, k);
            arena[(0xC0u + 4u * k) / 4u] = matrix_word(bias, k);
        }
        u32 r3 = out_at, r4 = a_at, r5 = b_at, r1 = sp_top;
        const u32 mode = static_cast<u32>(i % 10u);
        if (mode == 1u) r3 = a_at;
        else if (mode == 2u) r3 = b_at;
        else if (mode == 3u) r3 = a_at + 4u * (1u + rnd() % 14u);
        else if (mode == 4u) r1 = out_at + 0x30u + 4u * (rnd() % 8u);  // the frame lands on the output
        else if (mode == 5u) r4 = b_at;                                  // a and b the same matrix
        else if (mode == 6u) r5 = a_at + 4u * (rnd() % 16u);
        sync(p, arena, random_fpscr(), rnd(), r3, r4, r5, 0u);
        p.fast_side.cpu.gpr[1] = p.exact_side.cpu.gpr[1] = r1;
        wide_side.execute(p.fast_side.cpu);
        ordered_side.run(p.exact_side.cpu);
        compare_machines(p, "RwMatrixMultiply", mutate && (i % 7u == 0u));
        ++calls;
    }
    if (wide_out) *wide_out = wide_side.wide();
    return calls;
}

// Throughput of the old and new bodies, for the report (not a pass/fail condition).
template <class Fn>
double nanoseconds_per_call(Fn&& fn, u64 calls) {
    const auto start = std::chrono::steady_clock::now();
    for (u64 i = 0; i < calls; ++i) fn(i);
    const auto stop = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(stop - start).count() / static_cast<double>(calls);
}

void benchmarks() {
    Pair p;
    const std::vector<u32> arena = make_arena(16u);
    const u32 m_at = kArena + 0x80u, v_at = kArena + 0xC0u, out_at = kArena + 0x100u;
    sync(p, arena, 0u, 77u, out_at, m_at, v_at, single_to_double_bits(float_word(16u)));
    // ordinary matrix values
    for (u32 i = 0x20u; i < 0x80u; ++i) p.fast_side.memory.write32(kArena + 4u * i, float_to_bits(1.0f + 0.01f * static_cast<float>(i)));
    Cpu& c = p.fast_side.cpu;
    constexpr u64 kCalls = 2000000;
    const double point_new = nanoseconds_per_call([&](u64) { math_affine::serve_point(c); }, kCalls);
    const double point_old = nanoseconds_per_call([&](u64) { math_affine::serve_point_exact(c); }, kCalls);
    const double trans_new = nanoseconds_per_call([&](u64) { math_affine::serve_translate_by(c); }, kCalls);
    const double trans_old = nanoseconds_per_call([&](u64) { math_affine::serve_translate_by_exact(c); }, kCalls);
    c.gpr[3] = kArena + 0x80u, c.gpr[4] = kArena + 0xC0u;
    const double cross_new = nanoseconds_per_call([&](u64) { vec_math::serve_cross(c); }, kCalls);
    const double cross_old = nanoseconds_per_call([&](u64) { vec_math::serve_cross_exact(c); }, kCalls);
    const double len_new = nanoseconds_per_call([&](u64) { vec_math::serve_length(c); }, kCalls);
    const double len_old = nanoseconds_per_call([&](u64) { vec_math::serve_length_exact(c); }, kCalls);
    using namespace chara_colli_geometry;
    c.gpr[2] = kArena + 0x20u - kSegmentConstant;
    p.fast_side.memory.write32(kArena + 0x20u, 0u);
    const V3 centre{float_to_bits(1.5f), float_to_bits(2.5f), float_to_bits(-0.5f)};
    const V3 a{float_to_bits(0.0f), float_to_bits(0.0f), float_to_bits(0.0f)};
    const V3 b{float_to_bits(4.0f), float_to_bits(1.0f), float_to_bits(3.0f)};
    u64 sink = 0;
    const double dist_new = nanoseconds_per_call(
        [&](u64 k) {
            const V3 shifted{float_to_bits(1.5f + static_cast<float>(k & 7u) * 0.125f), centre.y, centre.z};
            sink += distance_squared(c, shifted, a, b);
        },
        kCalls);
    const double dist_old = nanoseconds_per_call(
        [&](u64 k) {
            const V3 shifted{float_to_bits(1.5f + static_cast<float>(k & 7u) * 0.125f), centre.y, centre.z};
            sink += distance_squared_exact(c, shifted, a, b);
        },
        kCalls);
    // compose_affine_matrix as the generated marshalling calls it: 24 scalar stores, the call, scalar reads of the result.
    {
        volatile u32 source[24];
        for (u32 k = 0; k < 24u; ++k) source[k] = float_to_bits(1.0f + 0.03125f * static_cast<float>(k));
        float values[24];
        float sink_f = 0.0f;
        const auto run_mode = [&](shadow::math::SubnormalMode mode) {
            return nanoseconds_per_call(
                [&](u64) {
                    for (u32 k = 0; k < 24u; ++k) {
                        const u32 w = source[k];
                        std::memcpy(values + k, &w, 4);
                    }
                    shadow::math::compose_affine_matrix(values, values + 12, mode);
                    for (u32 k = 0; k < 12u; ++k) sink_f += values[k] * 0.0f;
                },
                kCalls);
        };
        const double preserve = run_mode(shadow::math::SubnormalMode::Preserve);
        const double flush = run_mode(shadow::math::SubnormalMode::Flush);
        std::printf("compose_affine_matrix with marshalling stores: preserve %.1f ns, flush %.1f ns (sink %g)\n", preserve,
                    flush, static_cast<double>(sink_f));
    }
    std::printf("ns per call (new / old): point %.1f / %.1f, translate_by %.1f / %.1f, cross %.1f / %.1f, length %.1f / %.1f, "
                "distance_squared %.1f / %.1f (sink %llu)\n",
                point_new, point_old, trans_new, trans_old, cross_new, cross_old, len_new, len_old, dist_new, dist_old,
                static_cast<unsigned long long>(sink & 1u));
}

template <class Fn>
bool caught(Fn&& fn) {
    try {
        fn();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const u64 scale = argc > 1 ? static_cast<u64>(std::strtoull(argv[1], nullptr, 10)) : 1u;
        require(fast::kAvailable, "built without FMA: the fast routes are compiled out (nothing to test)");
        const Counts primitives = run_primitives(30000000ull * scale, 0u);
        run_compare(400000ull * scale, false);
        const u64 fast_taken = run_distance(3000000ull * scale, 0u);
        require(fast_taken * 3u > 3000000ull * scale, "distance_squared: the fast route was taken too rarely");
        const u64 casts_found = run_dynamic_cast(2000000ull * scale, 0u);
        require(casts_found > 100000u, "dynamic cast: too few successful casts for the test to mean anything");
        const u64 compose_clean = run_compose(3000000ull * scale, 0u);
        u64 wide_calls = 0;
        const u64 mm_calls = run_matrix_multiply(200000ull * scale, 0u, &wide_calls);
        require(wide_calls * 2u > mm_calls, "RwMatrixMultiply: the wide body was taken too rarely");
        const u64 affine = run_affine(40000ull * scale, 0u);
        const u64 vec = run_vec(40000ull * scale, 0u);
        require(caught([] { run_primitives(200000, 1u); }), "negative control: a perturbed fast primitive was not caught");
        require(caught([] { run_compare(20000, true); }), "negative control: a lost compare write was not caught");
        require(caught([] { run_distance(200000, 1u); }), "negative control: a perturbed distance was not caught");
        require(caught([] { run_dynamic_cast(2000, 1u); }), "negative control: a perturbed cast was not caught");
        require(caught([] { run_compose(2000, 1u); }), "negative control: a perturbed compose result was not caught");
        require(caught([] { run_matrix_multiply(2000, 1u, nullptr); }), "negative control: a perturbed matrix multiply was not caught");
        require(caught([] { run_affine(2000, 1u); }), "negative control: a perturbed affine result was not caught");
        require(caught([] { run_vec(2000, 1u); }), "negative control: a perturbed vec3 result was not caught");
        std::printf("PASS opt intrinsics diff: %llu primitive trials (%llu on the fast route), compare sequences, %llu successful casts, %llu "
                    "distance_squared calls (%llu on the fast route), %llu compose calls (%llu clean), %llu RwMatrixMultiply calls "
                    "(%llu wide), %llu affine and %llu vec3 whole-routine calls; 8 negative controls caught\n",
                    static_cast<unsigned long long>(primitives.total), static_cast<unsigned long long>(primitives.accepted),
                    static_cast<unsigned long long>(casts_found),
                    static_cast<unsigned long long>(3000000ull * scale), static_cast<unsigned long long>(fast_taken),
                    static_cast<unsigned long long>(3000000ull * scale), static_cast<unsigned long long>(compose_clean),
                    static_cast<unsigned long long>(mm_calls), static_cast<unsigned long long>(wide_calls),
                    static_cast<unsigned long long>(affine), static_cast<unsigned long long>(vec));
        benchmarks();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL opt intrinsics diff: %s\n", error.what());
        return 1;
    }
}
