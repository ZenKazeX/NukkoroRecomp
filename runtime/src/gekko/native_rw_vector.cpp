// RenderWare vector transforms, native. Contract: include/gekko/native_rw_vector.h.

#include "gekko/native_rw_vector.h"
#include "gekko/native_rw_refusal.h"

#include <cstdlib>
#include <cstring>

#include "gekko/cpu.h"
#include "gekko/ppc_ops.h"

namespace gekko::rw_vector {
namespace {

// A value as the psq_l of GQR0 = float makes it (a single widened to a double, subnormals as the CPU model defines them), and
// the psq_st that stores it back. The multiply-adds are the CPU model's own paired-single helpers (ppc::fps_mul / fps_madd:
// NaN selection, the FPSCR[NI] subnormal policy, one rounding to single), so the results are the guest instruction's.
u64 load(Cpu& c, u32 address) { return single_to_double_bits(c.mem_read32(address)); }
void store(Cpu& c, u32 address, u64 value) { c.mem_write32(address, double_to_single_bits_flush(value)); }

// The matrix rows RenderWare lays out as right (0), up (16), at (32), pos (48), each x y z (+ a pad word).
struct Matrix {
    u64 right[3], up[3], at[3], pos[3];
};

Matrix load_matrix(Cpu& c, u32 address, bool with_pos) {
    Matrix m{};
    for (u32 k = 0; k < 3u; ++k) {
        m.right[k] = load(c, address + 4u * k);
        m.up[k] = load(c, address + 16u + 4u * k);
        m.at[k] = load(c, address + 32u + 4u * k);
        if (with_pos) m.pos[k] = load(c, address + 48u + 4u * k);
    }
    return m;
}

struct Vector {
    u64 v[3];
};

Vector load_vector(Cpu& c, u32 address) { return Vector{{load(c, address), load(c, address + 4u), load(c, address + 8u)}}; }

// The state the originals' psq loads assume, and the FPU enabled (otherwise the guest body takes the FP-unavailable path).
bool servable(const Cpu& c) {
    const u32 gqr0 = c.spr[SPR_GQR0];
    return (c.msr() & 0x2000u) != 0u && ((gqr0 >> 16) & 7u) < 4u && (gqr0 & 7u) < 4u;
}

// out.k = at.k * z + (up.k * y + (right.k * x + pos.k)): ps_madds0 / ps_madds1 / ps_madds0.
Vector transform_point(Cpu& c, const Vector& p, const Matrix& m) {
    Vector out{};
    for (u32 k = 0; k < 3u; ++k) {
        u64 t = ppc::fps_madd(c, m.right[k], p.v[0], m.pos[k]);
        t = ppc::fps_madd(c, m.up[k], p.v[1], t);
        out.v[k] = ppc::fps_madd(c, m.at[k], p.v[2], t);
    }
    return out;
}

// out.k = at.k * z + (up.k * y + right.k * x): the first term is a plain product (ps_muls0).
Vector transform_vector(Cpu& c, const Vector& p, const Matrix& m) {
    Vector out{};
    for (u32 k = 0; k < 3u; ++k) {
        u64 t = ppc::fps_mul(c, m.right[k], p.v[0]);
        t = ppc::fps_madd(c, m.up[k], p.v[1], t);
        out.v[k] = ppc::fps_madd(c, m.at[k], p.v[2], t);
    }
    return out;
}

void store_vector(Cpu& c, u32 address, const Vector& v) {
    store(c, address, v.v[0]);
    store(c, address + 4u, v.v[1]);
    store(c, address + 8u, v.v[2]);
}

// ---- the square-root family -------------------------------------------------------------------------------------------------
// _rwSqrt / _rwInvSqrt are integer table lookups on the single's bit pattern (the tables are built by the engine's start-up
// code and reached through the engine instance, r13 + 0x615C, at the module offset r13 + 0x6140; the inverse table is the next
// pointer). The word a lookup produces is the result; nothing here is floating-point arithmetic.
constexpr u32 kSdaEngine = 0x615Cu, kSdaModule = 0x6140u, kSda2Zero = 0x1BACu;

u32 rotl(u32 v, u32 n) { return (v << n) | (v >> (32u - n)); }

u32 sqrt_word(Cpu& c, u32 word, bool inverse) {
    if (word == 0u) return 0u;
    const u32 biased = word + 0x800u;
    const u32 engine = c.mem_read32(c.gpr[13] + kSdaEngine);
    const u32 module = c.mem_read32(c.gpr[13] + kSdaModule) + (inverse ? 4u : 0u);
    const u32 table = c.mem_read32(engine + module);
    const u32 exponent = rotl(inverse ? ~biased : biased, 31u) & 0x3FC00000u;
    return exponent + c.mem_read32(table + (rotl(biased, 22u) & 0x3FFCu));
}

// f1 on entry (a double the stfs rounds to single), f1 on return (the looked-up word widened by the lfs)
u64 sqrt_value(Cpu& c, u64 f1, bool inverse) {
    return single_to_double_bits(sqrt_word(c, double_to_single_bits(f1), inverse));
}

void set_f1(Cpu& c, u64 value) {
    c.fpr[1].bits[0] = value;
    c.fpr[1].bits[1] = value;
}

bool fp_available(const Cpu& c) { return (c.msr() & 0x2000u) != 0u; }

}  // namespace

bool serve_rw_sqrt(Cpu& c) {
    if (!fp_available(c)) return rw_refusal::refuse(c, "_rwSqrt", "the FPU is off");
    set_f1(c, sqrt_value(c, c.fpr[1].bits[0], false));
    return true;
}

bool serve_rw_inv_sqrt(Cpu& c) {
    if (!fp_available(c)) return rw_refusal::refuse(c, "_rwInvSqrt", "the FPU is off");
    set_f1(c, sqrt_value(c, c.fpr[1].bits[0], true));
    return true;
}

bool serve_v3d_length(Cpu& c) {
    if (!fp_available(c)) return rw_refusal::refuse(c, "RwV3dLength", "the FPU is off");
    const u32 v = c.gpr[3];
    const u64 x2 = ppc::fps_mul(c, load(c, v), load(c, v)), y2 = ppc::fps_mul(c, load(c, v + 4u), load(c, v + 4u)),
              z2 = ppc::fps_mul(c, load(c, v + 8u), load(c, v + 8u));
    set_f1(c, sqrt_value(c, ppc::fps_add(c, z2, ppc::fps_add(c, x2, y2)), false));
    return true;
}

bool serve_v2d_length(Cpu& c) {
    if (!fp_available(c)) return rw_refusal::refuse(c, "RwV2dLength", "the FPU is off");
    const u32 v = c.gpr[3];
    const u64 x2 = ppc::fps_mul(c, load(c, v), load(c, v)), y2 = ppc::fps_mul(c, load(c, v + 4u), load(c, v + 4u));
    set_f1(c, sqrt_value(c, ppc::fps_add(c, x2, y2), false));
    return true;
}

// RwV3dNormalize(out = r3, in = r4): returns the length in f1. A length that is not above zero (and not a NaN) raises a
// RenderWare error in the original (after it has stored the scaled components): the owner declines before it changes anything,
// so the guest body runs that path whole. The original's f31 save / restore (psq_st / psq_l pair around an lfd restore) leaves
// f31's second slot rounded to a single, which is kept.
bool serve_v3d_normalize(Cpu& c) {
    if (!servable(c)) return rw_refusal::refuse(c, "RwV3dNormalize", "the FPU is off or GQR0 is not the float format");
    const u32 out = c.gpr[3], in = c.gpr[4];
    const u64 x = load(c, in), y = load(c, in + 4u), z = load(c, in + 8u);
    const u64 squares = ppc::fps_add(c, ppc::fps_mul(c, z, z), ppc::fps_add(c, ppc::fps_mul(c, x, x), ppc::fps_mul(c, y, y)));
    const u64 length = sqrt_value(c, squares, false);
    double length_value, zero_value;
    const u64 zero = single_to_double_bits(c.mem_read32(c.gpr[2] + kSda2Zero));
    std::memcpy(&length_value, &length, 8);
    std::memcpy(&zero_value, &zero, 8);
    if (length_value <= zero_value) return false;  // the error path
    const u64 inverse = sqrt_value(c, squares, true);
    c.mem_write32(out, double_to_single_bits(ppc::fps_mul(c, x, inverse)));
    c.mem_write32(out + 4u, double_to_single_bits(ppc::fps_mul(c, y, inverse)));
    c.mem_write32(out + 8u, double_to_single_bits(ppc::fps_mul(c, z, inverse)));
    c.fpr[31].bits[1] = single_to_double_bits(double_to_single_bits_flush(c.fpr[31].bits[1]));
    c.fpr[1].bits[0] = length;
    return true;
}

bool serve_transform_point(Cpu& c) {
    if (!servable(c)) return rw_refusal::refuse(c, "RwV3dTransformPoint", "the FPU is off or GQR0 is not the float format");
    const u32 out = c.gpr[3], in = c.gpr[4], matrix = c.gpr[5];
    const Vector p = load_vector(c, in);
    const Matrix m = load_matrix(c, matrix, true);
    store_vector(c, out, transform_point(c, p, m));
    return true;
}

bool serve_transform_vector(Cpu& c) {
    if (!servable(c)) return rw_refusal::refuse(c, "RwV3dTransformVector", "the FPU is off or GQR0 is not the float format");
    const u32 out = c.gpr[3], in = c.gpr[4], matrix = c.gpr[5];
    const Vector p = load_vector(c, in);
    const Matrix m = load_matrix(c, matrix, false);
    store_vector(c, out, transform_vector(c, p, m));
    return true;
}

bool serve_transform_points(Cpu& c) {
    if (!servable(c)) return rw_refusal::refuse(c, "RwV3dTransformPoints", "the FPU is off or GQR0 is not the float format");
    const u32 out = c.gpr[3], in = c.gpr[4], count = c.gpr[5], matrix = c.gpr[6];
    if (count == 0u) {
        c.request_stop(StopReason::ExecutionFault,
                       "RwV3dTransformPoints was called with a count of 0: the original's do-while loop would run 2^32 times");
        return true;
    }
    const Matrix m = load_matrix(c, matrix, true);
    Vector current = load_vector(c, in);
    for (u32 i = 0; i < count; ++i) {
        const Vector result = transform_point(c, current, m);
        // the software pipeline loads element i + 1 before it stores element i
        if (i + 1u < count) current = load_vector(c, in + 12u * (i + 1u));
        store_vector(c, out + 12u * i, result);
    }
    return true;
}

void install(Cpu& cpu) {
    cpu.install_hle(kTransformPoint, [](Cpu& c) { return serve_transform_point(c); }, "RenderWare RwV3dTransformPoint");
    cpu.install_hle(kTransformPoints, [](Cpu& c) { return serve_transform_points(c); }, "RenderWare RwV3dTransformPoints");
    cpu.install_hle(kRwSqrt, [](Cpu& c) { return serve_rw_sqrt(c); }, "RenderWare _rwSqrt");
    cpu.install_hle(kRwInvSqrt, [](Cpu& c) { return serve_rw_inv_sqrt(c); }, "RenderWare _rwInvSqrt");
    cpu.install_hle(kV3dLength, [](Cpu& c) { return serve_v3d_length(c); }, "RenderWare RwV3dLength");
    cpu.install_hle(kV2dLength, [](Cpu& c) { return serve_v2d_length(c); }, "RenderWare RwV2dLength");
    cpu.install_hle(kV3dNormalize, [](Cpu& c) { return serve_v3d_normalize(c); }, "RenderWare RwV3dNormalize");
    cpu.install_hle(kTransformVector, [](Cpu& c) { return serve_transform_vector(c); }, "RenderWare RwV3dTransformVector");
}

}  // namespace gekko::rw_vector
