// The game's 3-vector helpers, native. Contract: include/gekko/native_vec_math.h.

#include "gekko/native_vec_math.h"

#include <cmath>
#include <cstring>

#include "gekko/cpu.h"
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

void serve_sub(Cpu& cpu) noexcept {
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4], r5 = cpu.gpr[5];
    const SubnormalMode m = mode_of(cpu);
    const float ax = load(cpu, r4), bx = load(cpu, r5), ay = load(cpu, r4 + 4u), by = load(cpu, r5 + 4u),
                az = load(cpu, r4 + 8u), bz = load(cpu, r5 + 8u);
    const float x = d::subtract(ax, bx, m), y = d::subtract(ay, by, m), z = d::subtract(az, bz, m);
    store(cpu, r3, x);
    store(cpu, r3 + 4u, y);
    store(cpu, r3 + 8u, z);
}

void serve_add(Cpu& cpu) noexcept {
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4], r5 = cpu.gpr[5];
    const SubnormalMode m = mode_of(cpu);
    const float ax = load(cpu, r4), bx = load(cpu, r5), ay = load(cpu, r4 + 4u), by = load(cpu, r5 + 4u),
                az = load(cpu, r4 + 8u), bz = load(cpu, r5 + 8u);
    const float x = d::add(ax, bx, m), y = d::add(ay, by, m), z = d::add(az, bz, m);
    store(cpu, r3, x);
    store(cpu, r3 + 4u, y);
    store(cpu, r3 + 8u, z);
}

void serve_scale(Cpu& cpu) noexcept {
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4];
    const SubnormalMode m = mode_of(cpu);
    const u64 s = cpu.fpr[1].bits[0];
    const float ax = load(cpu, r4), ay = load(cpu, r4 + 4u), az = load(cpu, r4 + 8u);
    const float x = scale_one(ax, s, m), y = scale_one(ay, s, m), z = scale_one(az, s, m);
    store(cpu, r3, x);
    store(cpu, r3 + 4u, y);
    store(cpu, r3 + 8u, z);
}

void serve_add_to(Cpu& cpu) noexcept {  // 0x8040E048: the original stores x before it loads the destination's y and z
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4];
    const SubnormalMode m = mode_of(cpu);
    const float ax = load(cpu, r3), bx = load(cpu, r4), by = load(cpu, r4 + 4u), bz = load(cpu, r4 + 8u);
    store(cpu, r3, d::add(ax, bx, m));
    const float ay = load(cpu, r3 + 4u);
    store(cpu, r3 + 4u, d::add(ay, by, m));
    const float az = load(cpu, r3 + 8u);
    store(cpu, r3 + 8u, d::add(az, bz, m));
}

void serve_sub_from(Cpu& cpu) noexcept {  // 0x8040DE54
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4];
    const SubnormalMode m = mode_of(cpu);
    const float ax = load(cpu, r3), bx = load(cpu, r4), by = load(cpu, r4 + 4u), bz = load(cpu, r4 + 8u);
    store(cpu, r3, d::subtract(ax, bx, m));
    const float ay = load(cpu, r3 + 4u);
    store(cpu, r3 + 4u, d::subtract(ay, by, m));
    const float az = load(cpu, r3 + 8u);
    store(cpu, r3 + 8u, d::subtract(az, bz, m));
}

void serve_dot(Cpu& cpu) noexcept {  // 0x8040DFE0: y*y', then fmadds x, then fmadds z
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4];
    const SubnormalMode m = mode_of(cpu);
    const float ay = load(cpu, r3 + 4u), by = load(cpu, r4 + 4u), ax = load(cpu, r3), bx = load(cpu, r4),
                az = load(cpu, r3 + 8u), bz = load(cpu, r4 + 8u);
    float t = d::multiply(ay, by, m);
    t = d::fused(ax, bx, t, m);
    t = d::fused(az, bz, t, m);
    put_f1(cpu, t);
}

void serve_cross(Cpu& cpu) noexcept {  // 0x8040E008
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

void serve_length(Cpu& cpu) noexcept {  // 0x8040DFA0: fmuls x2, y2, z2; fadds (x2 + y2); fadds z2 + that; sqrt
    const u32 r3 = cpu.gpr[3];
    const SubnormalMode m = mode_of(cpu);
    const float x = load(cpu, r3), y = load(cpu, r3 + 4u), z = load(cpu, r3 + 8u);
    const float xx = d::multiply(x, x, m), yy = d::multiply(y, y, m), zz = d::multiply(z, z, m);
    const float sum = d::add(zz, d::add(xx, yy, m), m);
    put_f1(cpu, game_sqrt(sum, m));
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
    for (const Entry& e : kEntries) {
        cpu.install_hle(e.address, [serve = e.serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, e.name);
    }
}

}  // namespace gekko::vec_math
