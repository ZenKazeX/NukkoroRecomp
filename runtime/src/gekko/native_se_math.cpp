// The sound-effect manager's positional math, native (gekko/native_se_math.h).
//
// Every floating-point step names the instruction it stands for; operands are in the instruction's own order
// (fmadd frD = frA * frC + frB is ppc::fp_madd(c, A, C, B)), so the NaN priority and rounding are the original's.

#include "gekko/native_se_math.h"

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/ppc_ops.h"
#include "math/msl.h"

#include <array>

namespace gekko::se_math {

namespace {

// ---- constants (DOL addresses; dol_constants() lists them for the contract test) -------------------------------

// SE manager floats, K(x) = f32 at r2 + x.
constexpr u32 kZeroA = 0x00000000u;      // K(-0x38CC) 0x805F6EB4
constexpr u32 k127 = 0x42FE0000u;        // K(-0x38C8) 0x805F6EB8
constexpr u32 kMinus127 = 0xC2FE0000u;   // K(-0x38C0) 0x805F6EC0
constexpr u64 kMagicSigned = 0x4330000080000000ull;    // r2 - 0x38B8 0x805F6EC8 (double)
constexpr u32 k800 = 0x44480000u;        // K(-0x38B0) 0x805F6ED0
constexpr u32 k20 = 0x41A00000u;         // K(-0x38AC) 0x805F6ED4
constexpr u32 k6144 = 0x45C00000u;       // K(-0x38A8) 0x805F6ED8
constexpr u32 kMinus6144 = 0xC5C00000u;  // K(-0x38A4) 0x805F6EDC
constexpr u32 k16384 = 0x46800000u;      // K(-0x38A0) 0x805F6EE0
constexpr u32 kPan = 0x3F333333u;        // K(-0x389C) 0x805F6EE4, 0.7f
constexpr u32 k100A = 0x42C80000u;       // K(-0x3898) 0x805F6EE8
constexpr u32 k50 = 0x42480000u;         // K(-0x3894) 0x805F6EEC
constexpr u32 k60 = 0x42700000u;         // K(-0x3890) 0x805F6EF0
constexpr u32 kFar = 0x4B18967Fu;        // K(-0x3888) 0x805F6EF8, 9999999.0f
constexpr u32 kZeroB = 0x00000000u;      // K(-0x3884) 0x805F6EFC
constexpr u32 k400 = 0x43C80000u;        // K(-0x3880) 0x805F6F00
constexpr u32 k100B = 0x42C80000u;       // K(-0x387C) 0x805F6F04
constexpr u64 kMagicUnsigned = 0x4330000000000000ull;  // r2 - 0x3878 0x805F6F08 (double)

// sqrt 0x8000FF04 and Normalize 0x8040DEC4.
constexpr u32 kSqrtZeroF = 0x00000000u;              // r2 - 0x7F30 0x805F2850 (f32)
constexpr u64 kSqrtHalf = 0x3FE0000000000000ull;     // r2 - 0x7F18 0x805F2868
constexpr u64 kSqrtThree = 0x4008000000000000ull;    // r2 - 0x7F10 0x805F2870
constexpr u64 kSqrtZeroD = 0x0000000000000000ull;    // r2 - 0x7F08 0x805F2878
constexpr u32 kFloatNaN = 0x7FFFFFFFu;               // MSL __float_nan 0x805EED78
constexpr u32 kNormalizeEpsilon = 0x358637BDu;       // r2 + 0x167C 0x805FBDFC, 1e-6f

// ---- instruction-level helpers -----------------------------------------------------------------------------------

constexpr u32 kLT = 8u, kGT = 4u, kEQ = 2u;
constexpr u64 kSign = 0x8000000000000000ull;

u32 hi(u64 bits) noexcept { return static_cast<u32>(bits >> 32); }
u32 lo(u64 bits) noexcept { return static_cast<u32>(bits); }
u64 make(u32 high, u32 low) noexcept { return (static_cast<u64>(high) << 32) | low; }
u64 lfs(u32 word) noexcept { return single_to_double_bits(word); }
u32 stfs(u64 bits) noexcept { return double_to_single_bits(bits); }
u64 fneg(u64 bits) noexcept { return bits ^ kSign; }
u64 fabs_(u64 bits) noexcept { return bits & ~kSign; }
s32 fctiwz(Cpu& c, u64 bits) noexcept { return static_cast<s32>(lo(ppc::fctiw(c, bits, true))); }

// fcmpo / fcmpu: the condition code, and FPCC (plus the invalid-compare bits on NaN) into FPSCR, as the CPU does.
u32 compare(Cpu& c, u64 a, u64 b, bool ordered) noexcept {
    const double va = bits_to_double(a), vb = bits_to_double(b);
    u32 flags;
    if (va < vb) {
        flags = kLT;
    } else if (va > vb) {
        flags = kGT;
    } else if (va == vb) {
        flags = kEQ;
    } else {
        flags = 1u;
        ppc::fp_compare_unordered(c, a, b, ordered);
    }
    c.fpscr = (c.fpscr & ~kFpscrFPCC) | (flags << 12);
    return flags;
}
u32 fcmpo(Cpu& c, u64 a, u64 b) noexcept { return compare(c, a, b, true); }
u32 fcmpu(Cpu& c, u64 a, u64 b) noexcept { return compare(c, a, b, false); }

Vec3 read_vec(const GuestMemory& m, u32 address) noexcept {
    return {m.peek32(address), m.peek32(address + 4u), m.peek32(address + 8u)};
}

// ---- vector helpers ------------------------------------------------------------------------------------------------

// sqrt 0x8000FF04 (through 0x8000FEE4).
// For a positive finite single x the original's frsqrte seed, three Newton steps and frsp give exactly the host's
// correctly rounded single root: checked for all 2,139,095,039 positive finite singles against the sequence below,
// and se_math_contract_tests compares this function with the original routine. Only that case takes the host root;
// a double that is not a single, +infinity and every other case keep the original sequence.
u64 sqrt_guest(Cpu& c, u64 x) noexcept {
    if (fcmpo(c, x, lfs(kSqrtZeroF)) & kGT) {
        const double value = bits_to_double(x);
        const float single = static_cast<float>(value);
        if (static_cast<double>(single) == value && value < HUGE_VAL) {
            return double_to_bits(static_cast<double>(std::sqrt(single)));
        }
        u64 g = ppc::fp_rsqrte(c, x);                         // frsqrte f2, f1
        u64 f0 = ppc::fp_mul(c, g, g);                        // fmul f0, f2, f2
        g = ppc::fp_mul(c, kSqrtHalf, g);                     // fmul f2, f4, f2
        f0 = ppc::fp_nmsub(c, x, f0, kSqrtThree);             // fnmsub f0, f1, f0, f3
        g = ppc::fp_mul(c, g, f0);                            // fmul f2, f2, f0
        f0 = ppc::fp_mul(c, g, g);
        g = ppc::fp_mul(c, kSqrtHalf, g);
        f0 = ppc::fp_nmsub(c, x, f0, kSqrtThree);
        g = ppc::fp_mul(c, g, f0);
        f0 = ppc::fp_mul(c, g, g);
        g = ppc::fp_mul(c, kSqrtHalf, g);
        f0 = ppc::fp_nmsub(c, x, f0, kSqrtThree);
        f0 = ppc::fp_mul(c, g, f0);                           // fmul f0, f2, f0
        return ppc::fps_round(c, ppc::fp_mul(c, x, f0));      // fmul f1, f1, f0; frsp
    }
    if (fcmpo(c, x, kSqrtZeroD) & kLT) return lfs(kFloatNaN);
    // fpclassify of the stored single: only a NaN is replaced.
    const u32 word = stfs(x);
    if ((word & 0x7F800000u) == 0x7F800000u && (word & 0x007FFFFFu) != 0u) return lfs(kFloatNaN);
    return x;
}

// Sub 0x8040DDC4: a - b, stored.
Vec3 sub(Cpu& c, const Vec3& a, const Vec3& b) noexcept {
    return {stfs(ppc::fps_sub(c, lfs(a.x), lfs(b.x))), stfs(ppc::fps_sub(c, lfs(a.y), lfs(b.y))),
            stfs(ppc::fps_sub(c, lfs(a.z), lfs(b.z)))};
}

// Length 0x8040DFA0.
u64 length(Cpu& c, const Vec3& v) noexcept {
    const u64 x = lfs(v.x), y = lfs(v.y), z = lfs(v.z);
    const u64 x2 = ppc::fps_mul(c, x, x), y2 = ppc::fps_mul(c, y, y), z2 = ppc::fps_mul(c, z, z);
    return sqrt_guest(c, ppc::fps_add(c, z2, ppc::fps_add(c, x2, y2)));  // fadds f0, f1, f0; fadds f1, f2, f0
}

// ---- MSL (fdlibm) routines: the native library (math/msl.h, docs/NATIVE_MSL_MATH.md) in the caller's FPSCR[NI]
// mode. It returns values only; FPSCR's status bits are not part of the contract.
namespace msl = shadow::math::msl;
shadow::math::SubnormalMode mode_of(const Cpu& c) noexcept {
    return (c.fpscr & kFpscrNI) != 0u ? shadow::math::SubnormalMode::Flush : shadow::math::SubnormalMode::Preserve;
}
u64 sin_d(u64 x) noexcept { return double_to_bits(msl::sin(bits_to_double(x))); }
u64 cos_d(u64 x) noexcept { return double_to_bits(msl::cos(bits_to_double(x))); }
u64 atan_d(u64 x) noexcept { return double_to_bits(msl::atan(bits_to_double(x))); }
u64 atan2_d(Cpu& c, u64 y, u64 x) noexcept {
    return double_to_bits(msl::atan2(bits_to_double(y), bits_to_double(x), mode_of(c)));
}

// atan2f 0x80016260 and sinf 0x8000FFE8: the double routine, then frsp.
u64 atan2f_(Cpu& c, u64 y, u64 x) noexcept {
    return double_to_bits(double(msl::round_to_single(bits_to_double(atan2_d(c, y, x)), mode_of(c))));
}
u64 sinf_(Cpu& c, u64 x) noexcept {
    return double_to_bits(double(msl::round_to_single(bits_to_double(sin_d(x)), mode_of(c))));
}

// ---- guest inputs --------------------------------------------------------------------------------------------------

// The listener lookup (0x800742BC + 0x800A8F5C, then the weak pointer's lock): the first registry node whose actor
// has the id. `live` = the node's weak count block exists and its use count is not zero (0x8005232C).
struct Listener {
    bool live = false;
    u32 actor = 0;
};
Listener find_listener(const GuestMemory& m, u32 id) noexcept {
    const u32 registry = m.peek32(kRegistryPointer);
    if (registry == 0u) return {};  // the original would construct it empty (header)
    const u32 begin = m.peek32(registry + 8u);
    const u32 end = begin + (m.peek32(registry + 4u) << 3);
    for (u32 element = begin; element != end; element += 8u) {
        const u32 node = m.peek32(element);
        const u32 actor = m.peek32(node);
        if (m.peek32(actor + 0x298u) != id) continue;
        const u32 counts = m.peek32(node + 4u);
        return {counts != 0u && m.peek32(counts + 4u) != 0u, actor};
    }
    return {};
}

// Camera 0's transform (0x80009548, 0x8000D0F0 index 0): eye at +0, target at +0xC.
u32 camera_transform(const GuestMemory& m) noexcept {
    const u32 manager = m.peek32(kCameraPointer);
    return m.peek32(m.peek32(manager + 0xCu)) + 8u;
}

// The camera's yaw (0x80109F68 -> 0x8000A610 -> Normalize 0x8040DEC4 -> 0x804107B0), as PosPan reloads it.
u64 camera_yaw(Cpu& c, u32 transform) noexcept {
    const GuestMemory& m = c.guest_memory();
    Vec3 forward = sub(c, read_vec(m, transform + 0xCu), read_vec(m, transform));
    const u64 len = length(c, forward);
    if (fcmpo(c, len, lfs(kNormalizeEpsilon)) & kLT) {
        forward = read_vec(m, kZeroVector);
    } else {
        forward = {stfs(ppc::fps_div(c, lfs(forward.x), len)), stfs(ppc::fps_div(c, lfs(forward.y), len)),
                   stfs(ppc::fps_div(c, lfs(forward.z), len))};
    }
    const u64 fx = lfs(forward.x), fz = lfs(forward.z);
    const u64 r = sqrt_guest(c, ppc::fps_add(c, ppc::fps_mul(c, fx, fx), ppc::fps_mul(c, fz, fz)));
    const u64 yaw = atan2f_(c, fneg(fx), fneg(fz));
    (void)atan2f_(c, lfs(forward.y), r);  // the pitch: unused here, computed as the original does
    return lfs(stfs(yaw));
}

}  // namespace

u64 detail::msl_atan2(Cpu& cpu, u64 y, u64 x) noexcept { return atan2_d(cpu, y, x); }
u64 detail::msl_sin(Cpu&, u64 x) noexcept { return sin_d(x); }
u64 detail::msl_cos(Cpu&, u64 x) noexcept { return cos_d(x); }
u64 detail::msl_atan(Cpu&, u64 x) noexcept { return atan_d(x); }
u64 detail::guest_sqrt(Cpu& cpu, u64 x) noexcept { return sqrt_guest(cpu, x); }

// ---- exported ------------------------------------------------------------------------------------------------------

u64 listener_distance(Cpu& cpu, const Vec3& position) noexcept {
    const GuestMemory& m = cpu.guest_memory();
    if (m.peek8(kOutputConfig + 0xB2u) != 0u) {
        u64 first = lfs(kFar), second = first;
        const Listener one = find_listener(m, m.peek32(kPlayer1Id));
        if (one.live && one.actor != 0u) first = length(cpu, sub(cpu, read_vec(m, one.actor), position));
        const Listener two = find_listener(m, m.peek32(kPlayer2Id));
        if (two.live && two.actor != 0u) second = length(cpu, sub(cpu, read_vec(m, two.actor), position));
        return (fcmpo(cpu, first, second) & kLT) ? first : second;
    }
    const Listener one = find_listener(m, m.peek32(kPlayer1Id));
    if (one.live && one.actor != 0u) return length(cpu, sub(cpu, read_vec(m, one.actor), position));
    return lfs(kZeroB);
}

u64 listener_distance(Cpu& cpu, u32 position) noexcept {
    return listener_distance(cpu, read_vec(cpu.guest_memory(), position));
}

u64 max_distance_of(Cpu& cpu, u8 hundreds) noexcept {
    if (hundreds == 0u) return lfs(k400);
    return ppc::fps_mul(cpu, lfs(k100B), ppc::fps_sub(cpu, make(0x43300000u, hundreds), kMagicUnsigned));
}

u64 max_distance(Cpu& cpu, u32 row) noexcept { return max_distance_of(cpu, cpu.guest_memory().peek8(row + 0xCu)); }

s8 pos_volume(Cpu& cpu, const Vec3& position, u32 max_distance_bits, s8 volume_offset, s8 volume_ceiling) noexcept {
    const u64 limit = lfs(max_distance_bits);
    const u64 d = listener_distance(cpu, position);
    if (fcmpo(cpu, d, limit) & (kGT | kEQ)) return -127;  // cror eq, gt, eq
    const s32 mode = static_cast<s32>(cpu.guest_memory().peek32(kOutputConfig + 0x78u));
    const u64 near = lfs(k60);
    const u32 headroom = mode == 2 ? 30u : 0u;  // addic/subfe mask & 0x1E
    u32 a;
    if (fcmpo(cpu, d, near) & kLT) {
        a = 0u;
    } else {
        u64 t = ppc::fps_div(cpu, ppc::fps_sub(cpu, d, near), ppc::fps_sub(cpu, limit, near));
        if (fcmpo(cpu, t, lfs(kZeroA)) & kLT) t = lfs(kZeroA);
        const s32 base = static_cast<s16>(headroom);
        const u64 span =
            ppc::fps_sub(cpu, make(0x43300000u, static_cast<u32>(base + 127) ^ 0x80000000u), kMagicSigned);
        const u64 basef = ppc::fps_sub(cpu, make(0x43300000u, static_cast<u32>(base) ^ 0x80000000u), kMagicSigned);
        t = ppc::fps_mul(cpu, t, span);
        a = static_cast<u32>(fctiwz(cpu, ppc::fps_sub(cpu, basef, t)));
    }
    a += static_cast<u32>(static_cast<s32>(volume_offset));
    if (static_cast<s16>(a) < -127) a = static_cast<u32>(-127);
    if (mode == 2) {
        const s32 ceiling = static_cast<s32>(volume_ceiling) + static_cast<s16>(headroom);
        if (static_cast<s16>(a) > ceiling) a = static_cast<u32>(static_cast<s32>(static_cast<s16>(ceiling)));
        if (static_cast<s16>(a) > 127) a = 127u;
    } else if (static_cast<s16>(a) > static_cast<s32>(volume_ceiling)) {
        a = static_cast<u32>(static_cast<s32>(volume_ceiling));
    }
    return static_cast<s8>(static_cast<u8>(a));
}

s8 pos_volume(Cpu& cpu, u32 chan) noexcept {
    const GuestMemory& m = cpu.guest_memory();
    return pos_volume(cpu, read_vec(m, chan + kChanPosition), m.peek32(chan + kChanMaxDistance),
                      static_cast<s8>(m.peek8(chan + kChanVolumeOffset)),
                      static_cast<s8>(m.peek8(chan + kChanVolumeCeiling)));
}

s32 pos_pan(Cpu& cpu, const Vec3& position, s8* out_surround) noexcept {
    const GuestMemory& m = cpu.guest_memory();
    const s32 mode = static_cast<s32>(m.peek32(kOutputConfig + 0x78u));
    if (mode == 1) return 0;
    if (m.peek8(kOutputConfig + 0xB2u) != 0u) return 0;
    const u32 transform = camera_transform(m);
    const Vec3 eye = read_vec(m, transform);
    const u64 yaw = camera_yaw(cpu, transform);
    Vec3 d = sub(cpu, position, eye);
    d.y = stfs(lfs(kZeroA));
    const u64 r = length(cpu, d);
    if (mode == 2) {
        if (out_surround != nullptr) {
            (void)atan2f_(cpu, fneg(lfs(d.z)), fneg(lfs(d.x)));  // computed and discarded by the original
            *out_surround = static_cast<s8>(static_cast<u8>(fctiwz(cpu, lfs(kZeroA))));
        }
        return 0;
    }
    const u64 angle = ppc::fps_add(cpu, lfs(k16384), ppc::fps_add(cpu, atan2f_(cpu, lfs(d.z), lfs(d.x)), yaw));
    const u64 p = ppc::fps_mul(cpu, lfs(kPan), sinf_(cpu, angle));
    u64 v;
    if (fcmpo(cpu, r, lfs(k50)) & kLT) {
        v = ppc::fps_mul(cpu, lfs(k100A), ppc::fps_mul(cpu, p, ppc::fps_div(cpu, r, lfs(k50))));
    } else {
        v = ppc::fps_mul(cpu, lfs(k100A), p);
    }
    if (fcmpo(cpu, v, lfs(kMinus127)) & kLT) {
        v = lfs(kMinus127);
    } else if (fcmpo(cpu, v, lfs(k127)) & kGT) {
        v = lfs(k127);
    }
    return fctiwz(cpu, v);
}

s32 pos_pan(Cpu& cpu, u32 chan, s8* out_surround) noexcept {
    return pos_pan(cpu, read_vec(cpu.guest_memory(), chan + kChanPosition), out_surround);
}

s32 doppler_bend(Cpu& cpu, const Vec3& position, s16 bend, u32& previous_distance) noexcept {
    const GuestMemory& m = cpu.guest_memory();
    const u64 last_bend =
        ppc::fps_sub(cpu, make(0x43300000u, static_cast<u32>(static_cast<s32>(bend)) ^ 0x80000000u), kMagicSigned);
    Vec3 listener{stfs(lfs(kZeroA)), stfs(lfs(kZeroA)), stfs(lfs(kZeroA))};
    const Listener one = find_listener(m, m.peek32(kPlayer1Id));
    if (one.live) listener = read_vec(m, one.actor);
    const u64 d = length(cpu, sub(cpu, listener, position));
    const u64 zero = lfs(kZeroA), previous = lfs(previous_distance);
    if ((fcmpu(cpu, zero, previous) & kEQ) && !(fcmpu(cpu, zero, d) & kEQ)) {
        previous_distance = stfs(d);
        return 0;
    }
    if (fcmpo(cpu, previous, lfs(k800)) & kGT) {  // fcmpo f2, f0: the PREVIOUS distance
        previous_distance = stfs(d);
        return fctiwz(cpu, last_bend);
    }
    u64 b = ppc::fps_mul(cpu, lfs(k20), ppc::fps_sub(cpu, previous, d));
    if (fcmpo(cpu, b, lfs(k6144)) & kGT) {
        b = lfs(k6144);
    } else if (fcmpo(cpu, b, lfs(kMinus6144)) & kLT) {
        b = lfs(kMinus6144);
    }
    previous_distance = stfs(d);
    return fctiwz(cpu, b);
}

s32 doppler_bend(Cpu& cpu, u32 chan) noexcept {
    GuestMemory& m = cpu.guest_memory();
    u32 previous = m.peek32(chan + kChanPreviousDistance);
    const s32 result = doppler_bend(cpu, read_vec(m, chan + kChanPosition), static_cast<s16>(m.peek16(chan + kChanBend)),
                                    previous);
    m.write32(chan + kChanPreviousDistance, previous);
    return result;
}

const DolConstant* dol_constants(std::size_t& count) noexcept {
    // Addresses as r2 / r13 offsets (their absolute values are in the comments above each constant).
    static const auto kConstants = [] {
        std::array<DolConstant, 24> t{};
        std::size_t n = 0;
        const auto add = [&](u32 address, u32 size, u64 value) { t[n++] = {address, size, value}; };
        add(kR2 - 0x38CCu, 4, kZeroA); add(kR2 - 0x38C8u, 4, k127); add(kR2 - 0x38C0u, 4, kMinus127);
        add(kR2 - 0x38B8u, 8, kMagicSigned); add(kR2 - 0x38B0u, 4, k800); add(kR2 - 0x38ACu, 4, k20);
        add(kR2 - 0x38A8u, 4, k6144); add(kR2 - 0x38A4u, 4, kMinus6144); add(kR2 - 0x38A0u, 4, k16384);
        add(kR2 - 0x389Cu, 4, kPan); add(kR2 - 0x3898u, 4, k100A); add(kR2 - 0x3894u, 4, k50);
        add(kR2 - 0x3890u, 4, k60); add(kR2 - 0x3888u, 4, kFar); add(kR2 - 0x3884u, 4, kZeroB);
        add(kR2 - 0x3880u, 4, k400); add(kR2 - 0x387Cu, 4, k100B); add(kR2 - 0x3878u, 8, kMagicUnsigned);
        add(kR2 - 0x7F30u, 4, kSqrtZeroF); add(kR2 - 0x7F18u, 8, kSqrtHalf); add(kR2 - 0x7F10u, 8, kSqrtThree);
        add(kR2 - 0x7F08u, 8, kSqrtZeroD); add(kR13 + 0x2878u, 4, kFloatNaN); add(kR2 + 0x167Cu, 4, kNormalizeEpsilon);
        return n == t.size() ? t : std::array<DolConstant, 24>{};
    }();
    count = kConstants.size();
    return kConstants.data();
}

}  // namespace gekko::se_math
