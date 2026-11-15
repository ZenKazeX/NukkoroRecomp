// The game's 3-vector helpers, native. Contract: include/gekko/native_vec_math.h.

#include "gekko/native_vec_math.h"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <cstring>

#include "gekko/cpu.h"
#include "gekko/fast_single.h"
#include "gekko/ppc_ops.h"
#include "math/floating_point.h"

namespace gekko::vec_math {
namespace {
using shadow::math::SubnormalMode;
namespace d = shadow::math::detail;

constexpr u32 kFloatNaN = 0x7FFFFFFFu;  // MSL __float_nan (0x805EED78)

SubnormalMode mode_of(const Cpu& cpu) noexcept {
    return (cpu.fpscr & kFpscrNI) != 0u ? SubnormalMode::Flush : SubnormalMode::Preserve;
}
float load(Cpu& cpu, u32 address) noexcept { return d::value(cpu.mem_read32(address)); }
void store(Cpu& cpu, u32 address, float v) noexcept { cpu.mem_write32(address, d::bits(v)); }

// fmsubs frD,frA,frC,frB: a*c - b. NaN priority is a, b, c on the operands as given (a negated NaN b would flip its sign).
float fmsub(float a, float c, float b, SubnormalMode m) noexcept {
    if (d::nan(a)) return d::quiet(a);
    if (d::nan(b)) return d::quiet(b);
    return d::fused(a, c, -b, m);
}

// fmuls frD,frA,frC for Scale: f1 is a float argument, held in the register as a single (a frC that is not a single is outside the
// architecture and no executed site has one: docs/SHADOW_PLAYER_COVERAGE_AUDIT.md section 12).
float scale_one(float a, u64 s_bits, SubnormalMode m) noexcept {
    double s;
    std::memcpy(&s, &s_bits, sizeof(s));
    return d::multiply(a, static_cast<float>(s), m);
}

// The game's sqrt for a single argument (see the header).
float game_sqrt(float x, SubnormalMode m) noexcept {
    const u32 word = d::bits(x);
    if (d::nan(x)) return d::value(kFloatNaN);
    if ((word & 0x7FFFFFFFu) == 0u) return x;
    if ((word >> 31) != 0u) return d::value(kFloatNaN);
    if (word == 0x7F800000u) return d::value(0x7FC00000u);  // frsqrte(inf) = 0, then inf * 0 = the default NaN
    (void)m;
    return std::sqrt(x);
}

void put_f1(Cpu& cpu, float v) noexcept { cpu.fpr[1].bits[0] = single_to_double_bits(d::bits(v)); }
}  // namespace

void serve_sub_exact(Cpu& cpu) noexcept {
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4], r5 = cpu.gpr[5];
    const SubnormalMode m = mode_of(cpu);
    const float ax = load(cpu, r4), bx = load(cpu, r5), ay = load(cpu, r4 + 4u), by = load(cpu, r5 + 4u),
                az = load(cpu, r4 + 8u), bz = load(cpu, r5 + 8u);
    const float x = d::subtract(ax, bx, m), y = d::subtract(ay, by, m), z = d::subtract(az, bz, m);
    store(cpu, r3, x);
    store(cpu, r3 + 4u, y);
    store(cpu, r3 + 8u, z);
}

void serve_add_exact(Cpu& cpu) noexcept {
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4], r5 = cpu.gpr[5];
    const SubnormalMode m = mode_of(cpu);
    const float ax = load(cpu, r4), bx = load(cpu, r5), ay = load(cpu, r4 + 4u), by = load(cpu, r5 + 4u),
                az = load(cpu, r4 + 8u), bz = load(cpu, r5 + 8u);
    const float x = d::add(ax, bx, m), y = d::add(ay, by, m), z = d::add(az, bz, m);
    store(cpu, r3, x);
    store(cpu, r3 + 4u, y);
    store(cpu, r3 + 8u, z);
}

void serve_scale_exact(Cpu& cpu) noexcept {
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4];
    const SubnormalMode m = mode_of(cpu);
    const u64 s = cpu.fpr[1].bits[0];
    const float ax = load(cpu, r4), ay = load(cpu, r4 + 4u), az = load(cpu, r4 + 8u);
    const float x = scale_one(ax, s, m), y = scale_one(ay, s, m), z = scale_one(az, s, m);
    store(cpu, r3, x);
    store(cpu, r3 + 4u, y);
    store(cpu, r3 + 8u, z);
}

void serve_add_to_exact(Cpu& cpu) noexcept {  // 0x8040E048: the original stores x before it loads the destination's y and z
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4];
    const SubnormalMode m = mode_of(cpu);
    const float ax = load(cpu, r3), bx = load(cpu, r4), by = load(cpu, r4 + 4u), bz = load(cpu, r4 + 8u);
    store(cpu, r3, d::add(ax, bx, m));
    const float ay = load(cpu, r3 + 4u);
    store(cpu, r3 + 4u, d::add(ay, by, m));
    const float az = load(cpu, r3 + 8u);
    store(cpu, r3 + 8u, d::add(az, bz, m));
}

void serve_sub_from_exact(Cpu& cpu) noexcept {  // 0x8040DE54
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4];
    const SubnormalMode m = mode_of(cpu);
    const float ax = load(cpu, r3), bx = load(cpu, r4), by = load(cpu, r4 + 4u), bz = load(cpu, r4 + 8u);
    store(cpu, r3, d::subtract(ax, bx, m));
    const float ay = load(cpu, r3 + 4u);
    store(cpu, r3 + 4u, d::subtract(ay, by, m));
    const float az = load(cpu, r3 + 8u);
    store(cpu, r3 + 8u, d::subtract(az, bz, m));
}

void serve_dot_exact(Cpu& cpu) noexcept {  // 0x8040DFE0: y*y', then fmadds x, then fmadds z
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4];
    const SubnormalMode m = mode_of(cpu);
    const float ay = load(cpu, r3 + 4u), by = load(cpu, r4 + 4u), ax = load(cpu, r3), bx = load(cpu, r4),
                az = load(cpu, r3 + 8u), bz = load(cpu, r4 + 8u);
    float t = d::multiply(ay, by, m);
    t = d::fused(ax, bx, t, m);
    t = d::fused(az, bz, t, m);
    put_f1(cpu, t);
}

void serve_cross_exact(Cpu& cpu) noexcept {  // 0x8040E008
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4], r5 = cpu.gpr[5];
    const SubnormalMode m = mode_of(cpu);
    const float by = load(cpu, r5 + 4u), az = load(cpu, r4 + 8u), ax = load(cpu, r4), bz = load(cpu, r5 + 8u),
                ay = load(cpu, r4 + 4u), bx = load(cpu, r5);
    const float zy = d::multiply(az, by, m);
    const float xz = d::multiply(ax, bz, m);
    const float x = fmsub(ay, bz, zy, m);
    const float yx = d::multiply(ay, bx, m);
    const float y = fmsub(az, bx, xz, m);
    store(cpu, r3, x);
    const float z = fmsub(ax, by, yx, m);
    store(cpu, r3 + 4u, y);
    store(cpu, r3 + 8u, z);
}

void serve_length_exact(Cpu& cpu) noexcept {  // 0x8040DFA0: fmuls x2, y2, z2; fadds (x2 + y2); fadds z2 + that; sqrt
    const u32 r3 = cpu.gpr[3];
    const SubnormalMode m = mode_of(cpu);
    const float x = load(cpu, r3), y = load(cpu, r3 + 4u), z = load(cpu, r3 + 8u);
    const float xx = d::multiply(x, x, m), yy = d::multiply(y, y, m), zz = d::multiply(z, z, m);
    const float sum = d::add(zz, d::add(xx, yy, m), m);
    put_f1(cpu, game_sqrt(sum, m));
}

// ---- the plain-float routes (gekko/fast_single.h) ---------------------------------------------------------------------------
// Every operand is read first and every result computed and checked before the first store. The destination of AddTo /
// SubFrom is read word by word in the original, each after the previous store, but a store never lands on a later word
// of the same vector, so the order is not observable. A call off the ordinary range runs the original sequence above.
#if GEKKO_FAST_SINGLE
namespace {
struct Vec3 {
    float x, y, z;
};
GEKKO_ALWAYS_INLINE Vec3 read3(fast::Check& k, Cpu& cpu, u32 address) noexcept {
    const float x = k.in(cpu.mem_read32(address)), y = k.in(cpu.mem_read32(address + 4u)),
                z = k.in(cpu.mem_read32(address + 8u));
    return {x, y, z};
}
GEKKO_ALWAYS_INLINE void write3(Cpu& cpu, u32 address, float x, float y, float z) noexcept {
    cpu.mem_write32(address, d::bits(x));
    cpu.mem_write32(address + 4u, d::bits(y));
    cpu.mem_write32(address + 8u, d::bits(z));
}
GEKKO_ALWAYS_INLINE void set_f1(Cpu& cpu, float v) noexcept { cpu.fpr[1].bits[0] = fast::fpr_of(v); }
}  // namespace
#endif

void serve_sub(Cpu& cpu) noexcept {
#if GEKKO_FAST_SINGLE
    fast::Check k;
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4], r5 = cpu.gpr[5];
    const Vec3 a = read3(k, cpu, r4), b = read3(k, cpu, r5);
    const float x = k.out(a.x - b.x), y = k.out(a.y - b.y), z = k.out(a.z - b.z);
    if (k.ok()) {
        write3(cpu, r3, x, y, z);
        return;
    }
#endif
    serve_sub_exact(cpu);
}

void serve_add(Cpu& cpu) noexcept {
#if GEKKO_FAST_SINGLE
    fast::Check k;
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4], r5 = cpu.gpr[5];
    const Vec3 a = read3(k, cpu, r4), b = read3(k, cpu, r5);
    const float x = k.out(a.x + b.x), y = k.out(a.y + b.y), z = k.out(a.z + b.z);
    if (k.ok()) {
        write3(cpu, r3, x, y, z);
        return;
    }
#endif
    serve_add_exact(cpu);
}

void serve_scale(Cpu& cpu) noexcept {
#if GEKKO_FAST_SINGLE
    // f1 must be exactly an ordinary single (the original narrows it to float).
    const double sd = bits_to_double(cpu.fpr[1].bits[0]);
    const float sf = static_cast<float>(sd);
    if (static_cast<double>(sf) == sd) {
        fast::Check k;
        const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4];
        const float s = k.in(float_to_bits(sf));
        const Vec3 a = read3(k, cpu, r4);
        const float x = k.out(a.x * s), y = k.out(a.y * s), z = k.out(a.z * s);
        if (k.ok()) {
            write3(cpu, r3, x, y, z);
            return;
        }
    }
#endif
    serve_scale_exact(cpu);
}

void serve_add_to(Cpu& cpu) noexcept {
#if GEKKO_FAST_SINGLE
    fast::Check k;
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4];
    const Vec3 b = read3(k, cpu, r4), a = read3(k, cpu, r3);
    const float x = k.out(a.x + b.x), y = k.out(a.y + b.y), z = k.out(a.z + b.z);
    if (k.ok()) {
        write3(cpu, r3, x, y, z);
        return;
    }
#endif
    serve_add_to_exact(cpu);
}

void serve_sub_from(Cpu& cpu) noexcept {
#if GEKKO_FAST_SINGLE
    fast::Check k;
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4];
    const Vec3 b = read3(k, cpu, r4), a = read3(k, cpu, r3);
    const float x = k.out(a.x - b.x), y = k.out(a.y - b.y), z = k.out(a.z - b.z);
    if (k.ok()) {
        write3(cpu, r3, x, y, z);
        return;
    }
#endif
    serve_sub_from_exact(cpu);
}

void serve_dot(Cpu& cpu) noexcept {
#if GEKKO_FAST_SINGLE
    fast::Check k;
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4];
    const Vec3 a = read3(k, cpu, r3), b = read3(k, cpu, r4);
    float t = k.out(a.y * b.y);
    t = k.out(fast::fma(a.x, b.x, t));
    t = k.out(fast::fma(a.z, b.z, t));
    if (k.ok()) {
        set_f1(cpu, t);
        return;
    }
#endif
    serve_dot_exact(cpu);
}

void serve_cross(Cpu& cpu) noexcept {
#if GEKKO_FAST_SINGLE
    fast::Check k;
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4], r5 = cpu.gpr[5];
    const Vec3 a = read3(k, cpu, r4), b = read3(k, cpu, r5);
    const float zy = k.out(a.z * b.y);
    const float xz = k.out(a.x * b.z);
    const float yx = k.out(a.y * b.x);
    const float x = k.out(fast::fma(a.y, b.z, -zy));
    const float y = k.out(fast::fma(a.z, b.x, -xz));
    const float z = k.out(fast::fma(a.x, b.y, -yx));
    if (k.ok()) {
        write3(cpu, r3, x, y, z);
        return;
    }
#endif
    serve_cross_exact(cpu);
}

void serve_length(Cpu& cpu) noexcept {
#if GEKKO_FAST_SINGLE
    fast::Check k;
    const Vec3 v = read3(k, cpu, cpu.gpr[3]);
    const float xx = k.out(v.x * v.x), yy = k.out(v.y * v.y), zz = k.out(v.z * v.z);
    const float sum = k.out(zz + k.out(xx + yy));
    if (k.ok() && (d::bits(sum) & 0x7FFFFFFFu) != 0u) {  // a zero sum is returned as it is: the exact route
        set_f1(cpu, k.out(std::sqrt(sum)));
        if (k.ok()) return;
    }
#endif
    serve_length_exact(cpu);
}

// NEGATIVE CONTROL for tools/behaviour_diff.py (docs/agents/opt-behave-r3.md), test-only and OFF unless the environment sets
// GEKKO_BEHAVIOUR_NUDGE_LENGTH=1: Length returns its result one ULP up, the size of error a modern-math owner could introduce.
// A separate function installed only then, so the normal route costs nothing.
void serve_length_nudged(Cpu& cpu) noexcept {
    serve_length(cpu);
    double r;
    std::memcpy(&r, &cpu.fpr[1].bits[0], sizeof(r));
    const float up = std::nextafter(static_cast<float>(r), std::numeric_limits<float>::infinity());
    cpu.fpr[1].bits[0] = single_to_double_bits(d::bits(up));
}

void install(Cpu& cpu) {
    struct Entry {
        u32 address;
        void (*serve)(Cpu&) noexcept;
        const char* name;
    };
    static constexpr Entry kEntries[] = {
        {kSub, serve_sub, "native vec3: Sub"},       {kScale, serve_scale, "native vec3: Scale"},
        {kAdd, serve_add, "native vec3: Add"},       {kSubFrom, serve_sub_from, "native vec3: SubFrom"},
        {kLength, serve_length, "native vec3: Length"}, {kDot, serve_dot, "native vec3: Dot"},
        {kCross, serve_cross, "native vec3: Cross"}, {kAddTo, serve_add_to, "native vec3: AddTo"}};
    const char* const nudge = std::getenv("GEKKO_BEHAVIOUR_NUDGE_LENGTH");
    const bool nudge_length = nudge != nullptr && nudge[0] == '1';
    for (const Entry& e : kEntries) {
        cpu.install_hle(e.address, [serve = (nudge_length && e.address == kLength) ? serve_length_nudged : e.serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, e.name);
    }
}

}  // namespace gekko::vec_math
