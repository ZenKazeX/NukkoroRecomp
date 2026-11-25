// The native owners of the effect system's particle update (gekko/native_effect_update.h).

#include "gekko/native_effect_update.h"

#include <atomic>

#include "gekko/cpu.h"
#include "gekko/fast_single.h"
#include "gekko/native_guest_call.h"
#include "gekko/native_math_affine.h"
#include "gekko/native_vec_math.h"
#include "gekko/ppc_ops.h"

namespace gekko::effect_update {

namespace {
using F = u64;  // an FPR's double-format bits, exactly as the translated code holds the value

// .sdata2 constants the original loads (read from the guest image on every use, as the original does).
constexpr u32 kTinyConstant = 0x8051BC54u;   // 0.0001f
constexpr u32 kZeroConstant = 0x8051BC58u;   // 0.0f
constexpr u32 kMagicDouble = 0x8051B848u;    // 0x4330000080000000: 2^52 + 2^31, the int -> double trick's bias
constexpr u32 kPoolGlobal = 0x805E25C8u;     // the global the pool swap reads (a pointer to an object with a word at +4)

GEKKO_ALWAYS_INLINE F L(u32 word) noexcept { return single_to_double_bits(word); }
GEKKO_ALWAYS_INLINE u32 S(F value) noexcept { return double_to_single_bits(value); }

// fcmpo / fcmpu on two FPRs, reduced to the one condition the original branches on. The result code and FPCC are dead (no code
// reads them); an unordered compare also raises its FPSCR sticky bits, which the original's compare raises, so that case runs
// the CPU's own compare.
GEKKO_ALWAYS_INLINE bool greater(Cpu& c, F a, F b, bool ordered) noexcept {
    const double x = bits_to_double(a), y = bits_to_double(b);
    if (x > y) return true;
    if (x < y || x == y) return false;
    ppc::fp_compare(c, 0u, a, b, ordered);
    return false;
}
GEKKO_ALWAYS_INLINE bool less(Cpu& c, F a, F b, bool ordered) noexcept {
    const double x = bits_to_double(a), y = bits_to_double(b);
    if (x < y) return true;
    if (x > y || x == y) return false;
    ppc::fp_compare(c, 0u, a, b, ordered);
    return false;
}
GEKKO_ALWAYS_INLINE bool equal(Cpu& c, F a, F b, bool ordered) noexcept {
    const double x = bits_to_double(a), y = bits_to_double(b);
    if (x == y) return true;
    if (x < y || x > y) return false;
    ppc::fp_compare(c, 0u, a, b, ordered);
    return false;
}

// ---- the same arithmetic in plain single precision (gekko/fast_single.h) ------------------------------------------------
// Every operation below is the exact call it stands for - ppc::fps_* on FPR bit patterns, then the store conversion - with
// one arm in front of it: when each operand is zero or an ordinary single and so is the result, the host's own float
// operation gives the same bits (fast_single.h says why), and there is nothing else to do: fps_* writes no state and reads
// FPSCR[NI] only for the values the test excludes. Memory is read and written in exactly the order of the exact form; only
// the route a value takes differs.
struct Num {
    F bits = 0;      // the FPR image: valid when !ok
    float f = 0.0f;  // valid when ok: the value, zero or an ordinary single
    bool ok = false;
};
GEKKO_ALWAYS_INLINE F bits_of(const Num& n) noexcept { return n.ok ? fast::fpr_of(n.f) : n.bits; }
GEKKO_ALWAYS_INLINE Num exact(F bits) noexcept { return Num{bits, 0.0f, false}; }
GEKKO_ALWAYS_INLINE Num plain(float f) noexcept { return Num{0u, f, true}; }
// An FPR value: plain when it is exactly a single that is zero or ordinary.
GEKKO_ALWAYS_INLINE Num num(F bits) noexcept {
    const double d = bits_to_double(bits);
    const float f = static_cast<float>(d);
    return Num{bits, f, static_cast<double>(f) == d && fast::bad_word(float_to_bits(f)) == 0u};
}
GEKKO_ALWAYS_INLINE Num num_word(u32 w) noexcept {
    if (fast::bad_word(w) == 0u) return plain(bits_to_float(w));
    return exact(L(w));
}

// S(fps_add(L(w), b))
GEKKO_ALWAYS_INLINE u32 add_word(Cpu& c, u32 w, const Num& b) noexcept {
    if (b.ok) {
        fast::Check k;
        const float r = k.out(k.in(w) + b.f);
        if (k.ok()) return float_to_bits(r);
    }
    return S(ppc::fps_add(c, L(w), bits_of(b)));
}
// fps_add(L(a), L(b))
GEKKO_ALWAYS_INLINE Num add_words(Cpu& c, u32 a, u32 b) noexcept {
    fast::Check k;
    const float r = k.out(k.in(a) + k.in(b));
    if (k.ok()) return plain(r);
    return exact(ppc::fps_add(c, L(a), L(b)));
}
// fps_mul(L(w), b)
GEKKO_ALWAYS_INLINE Num mul_word(Cpu& c, u32 w, const Num& b) noexcept {
    if (b.ok) {
        fast::Check k;
        const float r = k.out(k.in(w) * b.f);
        if (k.ok()) return plain(r);
    }
    return exact(ppc::fps_mul(c, L(w), bits_of(b)));
}
// S(fps_mul(L(w), b))
GEKKO_ALWAYS_INLINE u32 mul_store(Cpu& c, u32 w, const Num& b) noexcept {
    if (b.ok) {
        fast::Check k;
        const float r = k.out(k.in(w) * b.f);
        if (k.ok()) return float_to_bits(r);
    }
    return S(ppc::fps_mul(c, L(w), bits_of(b)));
}
// fps_mul(a, L(w))
GEKKO_ALWAYS_INLINE Num mul_by_word(Cpu& c, const Num& a, u32 w) noexcept {
    if (a.ok) {
        fast::Check k;
        const float r = k.out(a.f * k.in(w));
        if (k.ok()) return plain(r);
    }
    return exact(ppc::fps_mul(c, bits_of(a), L(w)));
}
GEKKO_ALWAYS_INLINE Num mul(Cpu& c, const Num& a, const Num& b) noexcept {
    if (a.ok & b.ok) {
        fast::Check k;
        const float r = k.out(a.f * b.f);
        if (k.ok()) return plain(r);
    }
    return exact(ppc::fps_mul(c, bits_of(a), bits_of(b)));
}
GEKKO_ALWAYS_INLINE Num sub(Cpu& c, const Num& a, const Num& b) noexcept {
    if (a.ok & b.ok) {
        fast::Check k;
        const float r = k.out(a.f - b.f);
        if (k.ok()) return plain(r);
    }
    return exact(ppc::fps_sub(c, bits_of(a), bits_of(b)));
}
GEKKO_ALWAYS_INLINE Num div(Cpu& c, const Num& a, const Num& b) noexcept {
    if (a.ok & b.ok) {
        fast::Check k;
        const float r = k.out(a.f / b.f);
        if (k.ok()) return plain(r);
    }
    return exact(ppc::fps_div(c, bits_of(a), bits_of(b)));
}
// S(fps_madd(L(a), cc, L(b))): a * cc + b, rounded once
GEKKO_ALWAYS_INLINE u32 madd_word(Cpu& c, u32 a, const Num& cc, u32 b) noexcept {
    if (fast::kAvailable && cc.ok) {
        fast::Check k;
        const float r = k.out(fast::fma(k.in(a), cc.f, k.in(b)));
        if (k.ok()) return float_to_bits(r);
    }
    return S(ppc::fps_madd(c, L(a), bits_of(cc), L(b)));
}
// S(fps_nmsub(a, L(cc), L(b))): -(a * cc - b), the product and the difference rounded once, then the sign turned
GEKKO_ALWAYS_INLINE u32 nmsub_word(Cpu& c, const Num& a, u32 cc, u32 b) noexcept {
    if (fast::kAvailable && a.ok) {
        fast::Check k;
        const float r = k.out(fast::fma(a.f, k.in(cc), -k.in(b)));
        if (k.ok()) return float_to_bits(r) ^ 0x80000000u;
    }
    return S(ppc::fps_nmsub(c, bits_of(a), L(cc), L(b)));
}

// The compares on plain values: two ordinary singles compare as their doubles do. Anything else is the exact compare.
GEKKO_ALWAYS_INLINE bool greater(Cpu& c, const Num& a, const Num& b, bool ordered) noexcept {
    if (a.ok & b.ok) return a.f > b.f;
    return greater(c, bits_of(a), bits_of(b), ordered);
}
GEKKO_ALWAYS_INLINE bool greater_word(Cpu& c, u32 w, const Num& b, bool ordered) noexcept {
    if (b.ok && fast::bad_word(w) == 0u) return bits_to_float(w) > b.f;
    return greater(c, L(w), bits_of(b), ordered);
}
GEKKO_ALWAYS_INLINE bool greater_than_word(Cpu& c, const Num& a, u32 w, bool ordered) noexcept {
    if (a.ok && fast::bad_word(w) == 0u) return a.f > bits_to_float(w);
    return greater(c, bits_of(a), L(w), ordered);
}
GEKKO_ALWAYS_INLINE bool greater_words(Cpu& c, u32 a, u32 b, bool ordered) noexcept {
    if ((fast::bad_word(a) | fast::bad_word(b)) == 0u) return bits_to_float(a) > bits_to_float(b);
    return greater(c, L(a), L(b), ordered);
}
GEKKO_ALWAYS_INLINE bool equal_word(Cpu& c, const Num& a, u32 w, bool ordered) noexcept {
    if (a.ok && fast::bad_word(w) == 0u) return a.f == bits_to_float(w);
    return equal(c, bits_of(a), L(w), ordered);
}

// ---- the commands -------------------------------------------------------------------------------------------------------

// The command was spent or ran out: when its data record's byte +2 is set, the unit's flags word (the pointer at +0x14) gets
// bit 0 (the unit is finished).
GEKKO_ALWAYS_INLINE void finish(Cpu& c, u32 self) noexcept {
    const u32 record = c.mem_read32(self + 4u);
    if (c.mem_read8(record + 2u) != 0u) {
        const u32 flags = c.mem_read32(self + 0x14u);
        c.mem_write32(flags, c.mem_read32(flags) | 1u);
    }
}

enum class Step { Finished, Alive, Run };

// The part every update shares: no target = finished; the first call only marks the command started; then elapsed += dt and
// the command is finished once it exceeds its duration. Run: the update's own work follows.
GEKKO_ALWAYS_INLINE Step prologue(Cpu& c, u32 self, const Num& dt) noexcept {
    if (c.mem_read32(self + 0x18u) == 0u) {
        finish(c, self);
        return Step::Finished;
    }
    if (c.mem_read8(self + 0x10u) == 0u) {
        c.mem_write8(self + 0x10u, 1u);
        return Step::Alive;
    }
    c.mem_write32(self + 0x1Cu, add_word(c, c.mem_read32(self + 0x1Cu), dt));
    const u32 elapsed = c.mem_read32(self + 0x1Cu);
    const u32 duration = c.mem_read32(self + 0x20u);
    if (greater_words(c, elapsed, duration, true)) {
        finish(c, self);
        return Step::Finished;
    }
    return Step::Run;
}

bool alpha_ramp(Cpu& c, u32 self, const Num& dt) noexcept {
    const Step step = prologue(c, self, dt);
    if (step != Step::Run) return step == Step::Alive;
    const u32 rate = c.mem_read32(self + 0x24u);
    if (rate == 0u) return true;
    // (double)rate through the 2^52 bias, rounded to a single by fsubs, scaled by dt, truncated to an int. With the bias
    // the game has there, the difference is the rate as a signed integer, exactly, and its rounding to a single is the
    // host's int -> float conversion (the rate is not 0 here, so the value is at least 1 in magnitude).
    const F biased = 0x4330000000000000ull | static_cast<u64>(rate ^ 0x80000000u);
    const u64 bias = c.mem_read64(kMagicDouble);
    const Num as_float = bias == 0x4330000080000000ull ? plain(static_cast<float>(static_cast<s32>(rate)))
                                                        : exact(ppc::fps_sub(c, biased, bias));
    const F delta = bits_of(mul(c, as_float, dt));
    const u32 step_value = static_cast<u32>(ppc::fctiw(c, delta, true));
    const u32 cell = c.mem_read32(self + 0x18u);
    const u32 before = c.mem_read32(cell);
    c.mem_write32(cell, before + step_value);
    const u32 cell_again = c.mem_read32(self + 0x18u);
    const s32 value = static_cast<s32>(c.mem_read32(cell_again));
    constexpr s32 kMax = 0x3FC00000;   // 255.0 in 10.22 fixed point
    constexpr s32 kMin = 0x3FFFFF;
    if (value > kMax) {
        c.mem_write32(cell_again, static_cast<u32>(kMax));
        c.mem_write32(self + 0x24u, 0u);
    } else if (value < kMin) {
        c.mem_write32(self + 0x18u, 0u);
        c.mem_write32(self + 0x24u, 0u);
    }
    return true;
}

bool scalar_update(Cpu& c, u32 self, const Num& dt) noexcept {
    const Step step = prologue(c, self, dt);
    if (step != Step::Run) return step == Step::Alive;
    const u32 cell = c.mem_read32(self + 0x18u);
    const u32 rate = c.mem_read32(self + 0x24u);
    const u32 current = c.mem_read32(cell);
    c.mem_write32(cell, madd_word(c, rate, dt, current));
    return true;
}

// target[i] += rate[i] * dt: the rates are all read (scaled) first, then each target word is read, added and stored in turn
// (0x800091A4 into a frame temporary, then 0x8040E048).
GEKKO_ALWAYS_INLINE void add_scaled(Cpu& c, u32 target, u32 vector, const Num& dt) noexcept {
    const Num x = mul_word(c, c.mem_read32(vector), dt);
    const Num y = mul_word(c, c.mem_read32(vector + 4u), dt);
    const Num z = mul_word(c, c.mem_read32(vector + 8u), dt);
    c.mem_write32(target, add_word(c, c.mem_read32(target), x));
    c.mem_write32(target + 4u, add_word(c, c.mem_read32(target + 4u), y));
    c.mem_write32(target + 8u, add_word(c, c.mem_read32(target + 8u), z));
}

bool vector_update(Cpu& c, u32 self, const Num& dt) noexcept {
    const Step step = prologue(c, self, dt);
    if (step != Step::Run) return step == Step::Alive;
    add_scaled(c, c.mem_read32(self + 0x18u), self + 0x24u, dt);
    return true;
}

// ---- the unit step ------------------------------------------------------------------------------------------------------

// The original's return addresses after its three kinds of nested call (never fetched: guest_call needs an address inside the
// original function to return to).
constexpr u32 kLinkScript = 0x804280F8u;   // liveness-roots: value-only
constexpr u32 kLinkUpdate = 0x80428124u;   // liveness-roots: value-only
constexpr u32 kLinkDelete1 = 0x804281A0u;  // liveness-roots: value-only
constexpr u32 kLinkDelete2 = 0x80428254u;  // liveness-roots: value-only
constexpr u32 kScriptStep = 0x80429224u;

struct Frame {
    Cpu& c;
    u32 self;
    F dt;
    Num step;  // dt, plain when it is an ordinary single
    bool failed = false;
};

// A deleting destructor call: vtable slot +8 with flag 1.
void destroy(Frame& f, u32 object, u32 link) {
    if (f.failed) return;
    const u32 target = f.c.mem_read32(f.c.mem_read32(object) + 8u);
    u32 result;
    if (!guest_call(f.c, target, link, {object, 1u}, result)) f.failed = true;
}

// A command's update: the three known classes inline, any other through the game's own function.
bool update_command(Frame& f, u32 command) {
    Cpu& c = f.c;
    const u32 target = c.mem_read32(c.mem_read32(command) + 0xCu);
    switch (target) {
        case kAlphaRamp: return alpha_ramp(c, command, f.step);
        case kScalar: return scalar_update(c, command, f.step);
        case kVector:
        case kVector2D: return vector_update(c, command, f.step);
        default: break;
    }
    c.fpr[1].bits[0] = f.dt;
    u32 result;
    if (!guest_call(c, target, kLinkUpdate, {command}, result)) {
        f.failed = true;
        return true;  // stops the walk: the run is already stopped
    }
    return (result & 0xFFu) != 0u;
}

// Unit lists: the unit's commands, each updated, the finished ones unlinked and deleted.
void run_commands(Frame& f, u32 unit) {
    Cpu& c = f.c;
    u32 command = c.mem_read32(unit + 0x70u);
    while (command != 0u && !f.failed) {
        if (update_command(f, command)) {
            if (f.failed) return;
            command = c.mem_read32(command + 0xCu);
            continue;
        }
        const u32 prev = c.mem_read32(command + 8u);
        if (prev != 0u) c.mem_write32(prev + 0xCu, c.mem_read32(command + 0xCu));
        const u32 next = c.mem_read32(command + 0xCu);
        if (next != 0u) c.mem_write32(next + 8u, c.mem_read32(command + 8u));
        if (command == c.mem_read32(unit + 0x70u)) c.mem_write32(unit + 0x70u, c.mem_read32(command + 0xCu));
        if (command == c.mem_read32(unit + 0x74u)) c.mem_write32(unit + 0x74u, c.mem_read32(command + 8u));
        const u32 following = c.mem_read32(command + 0xCu);
        destroy(f, command, kLinkDelete1);
        command = following;
    }
}

// The pool swap: the global's object gets the system's pool word in its +4 (the original returns the old word; no caller reads it).
GEKKO_ALWAYS_INLINE void pool_swap(Cpu& c, u32 self, u32 word_offset) noexcept {
    const u32 pool = c.mem_read32(kPoolGlobal);
    const u32 word = c.mem_read32(self + word_offset);
    c.mem_write32(pool + 4u, word);
}

GEKKO_ALWAYS_INLINE void scale_in_place(Cpu& c, u32 vector, const Num& factor) noexcept {
    for (u32 lane = 0; lane < 3u; ++lane)
        c.mem_write32(vector + 4u * lane, mul_store(c, c.mem_read32(vector + 4u * lane), factor));
}
}  // namespace

void serve_alpha_ramp(Cpu& c) {
    c.gpr[3] = alpha_ramp(c, c.gpr[3], num(c.fpr[1].bits[0])) ? 1u : 0u;
}

void serve_scalar(Cpu& c) {
    c.gpr[3] = scalar_update(c, c.gpr[3], num(c.fpr[1].bits[0])) ? 1u : 0u;
}

void serve_vector(Cpu& c) {
    c.gpr[3] = vector_update(c, c.gpr[3], num(c.fpr[1].bits[0])) ? 1u : 0u;
}

void serve_unit_step(Cpu& c) {
    Frame f{c, c.gpr[3], c.fpr[1].bits[0], num(c.fpr[1].bits[0])};
    const u32 self = f.self;
    const Num dt = f.step;

    // 1. age the units (unless the definition says there is no limit) and flag the ones past the limit
    if ((c.mem_read32(self + 0x108u) & 0x100u) == 0u) {
        const u32 tiny = c.mem_read32(kTinyConstant);
        const Num limit = add_words(c, tiny, c.mem_read32(c.mem_read32(self + 0x124u) + 0xCu));
        for (u32 unit = c.mem_read32(self + 0xFCu); unit != 0u; unit = c.mem_read32(unit + 0x68u)) {
            if ((c.mem_read32(self + 0x18u) & 2u) != 0u)  // the system's flag, read again for every unit
                c.mem_write32(unit + 0x50u, add_word(c, c.mem_read32(unit + 0x50u), dt));
            if (greater_word(c, c.mem_read32(unit + 0x50u), limit, true))
                c.mem_write32(unit + 0x4Cu, c.mem_read32(unit + 0x4Cu) | 1u);
        }
    }

    // 2. the scripts and the command lists of the units that are not finished
    pool_swap(c, self, 0x134u);
    for (u32 unit = c.mem_read32(self + 0xFCu); unit != 0u; unit = c.mem_read32(unit + 0x68u)) {
        if ((c.mem_read32(unit + 0x4Cu) & 1u) != 0u) continue;
        if (c.mem_read32(unit + 0x6Cu) != 0u) {  // the script step does nothing without a cursor
            u32 result;
            if (!guest_call(c, kScriptStep, kLinkScript, {unit}, result)) f.failed = true;
            if (f.failed) break;
        }
        if ((c.mem_read32(unit + 0x4Cu) & 1u) != 0u) continue;
        run_commands(f, unit);
        if (f.failed) break;
    }
    if (f.failed) {
        c.request_stop(StopReason::ExecutionFault, "effect unit step: a nested guest call did not return");
        return;
    }

    // 3. unlink and delete the finished units
    pool_swap(c, self, 0x138u);
    for (u32 unit = c.mem_read32(self + 0xFCu); unit != 0u && !f.failed;) {
        const u32 next = c.mem_read32(unit + 0x68u);
        if ((c.mem_read32(unit + 0x4Cu) & 1u) != 0u) {
            const u32 prev = c.mem_read32(unit + 0x64u);
            if (prev != 0u) c.mem_write32(prev + 0x68u, next);
            const u32 after = c.mem_read32(unit + 0x68u);
            if (after != 0u) c.mem_write32(after + 0x64u, c.mem_read32(unit + 0x64u));
            if (c.mem_read32(self + 0xFCu) == unit) c.mem_write32(self + 0xFCu, c.mem_read32(unit + 0x68u));
            if (c.mem_read32(self + 0x100u) == unit) c.mem_write32(self + 0x100u, c.mem_read32(unit + 0x64u));
            destroy(f, unit, kLinkDelete2);
            c.mem_write32(self + 0x110u, c.mem_read32(self + 0x110u) - 1u);
        }
        unit = next;
    }
    if (f.failed) {
        c.request_stop(StopReason::ExecutionFault, "effect unit step: a nested guest call did not return");
        return;
    }

    // 4. move the units: position += velocity * dt; the drag blend; the gravity pull on the velocity's y
    const Num zero = num_word(c.mem_read32(kZeroConstant));
    for (u32 unit = c.mem_read32(self + 0xFCu); unit != 0u; unit = c.mem_read32(unit + 0x68u)) {
        add_scaled(c, unit + 4u, unit + 0x1Cu, dt);
        const u32 drag = c.mem_read32(self + 0x118u);
        if (!equal_word(c, zero, drag, false)) {
            c.gpr[3] = unit + 0x1Cu;
            vec_math::serve_length(c);  // the game's length: f1
            const Num length = num(c.fpr[1].bits[0]);
            if (greater_than_word(c, length, c.mem_read32(kTinyConstant), true)) {
                const Num damped = mul_by_word(c, length, c.mem_read32(self + 0x118u));
                if (greater(c, damped, length, true)) {
                    scale_in_place(c, unit + 0x1Cu, num_word(c.mem_read32(kZeroConstant)));
                } else {
                    const Num kept = sub(c, length, damped);
                    scale_in_place(c, unit + 0x1Cu, div(c, kept, length));
                }
            }
        }
        const u32 gravity = c.mem_read32(self + 0x114u);
        if (!equal_word(c, zero, gravity, false)) {
            const u32 y = c.mem_read32(unit + 0x20u);
            c.mem_write32(unit + 0x20u, nmsub_word(c, dt, gravity, y));
        }
    }
}

// ---- the particle renderer's vertex fill (0x80434824) -------------------------------------------------------------------

namespace {
constexpr u32 kPiConstant = 0x805FBDB0u;       // 3.14159274f: the rotation is wrapped into [-pi, pi]
constexpr u32 kTwoConstant = 0x8051C798u;      // 2.0f
constexpr u32 kZeroFloat = 0x8051C790u;        // 0.0f
constexpr u32 kSmallDataTankOffset = 0x5FF0u;  // r13 + this: the word holding the tank object's attribute-table offset
constexpr u32 kWrapLimit = 1u << 16;           // wraps per unit before the native fill declines (the original loops without bound)

// The tank attribute lock (RenderWare 0x80449458 with lock flags 0x40000000: a leaf over memory with no frame, no loop and no
// device access) and unlock (0x804495E8), inlined with the registers they leave dead. The {pointer, stride} of the attribute
// stream is written at `out`; the attribute's lock bits are or-ed into the tank object's +0x3C word.
GEKKO_ALWAYS_INLINE u32 tank_object(Cpu& c, u32 tank) noexcept {
    return c.mem_read32(tank + c.mem_read32(c.gpr[13] + kSmallDataTankOffset));
}
GEKKO_ALWAYS_INLINE void lock_stream(Cpu& c, u32 tank, u32 attribute, u32 offset_pointer, u32 offset_stride, u32 out) noexcept {
    const u32 object = tank_object(c, tank);
    if ((c.mem_read32(object + 0xA0u) & attribute) == 0u) {
        c.mem_write32(out, 0u);
        c.mem_write32(out + 4u, 0u);
        return;
    }
    const u32 pointer = c.mem_read32(object + offset_pointer);
    const u32 stride = c.mem_read32(object + offset_stride);
    c.mem_write32(out, pointer);
    c.mem_write32(out + 4u, stride);
    const u32 override_stride = c.mem_read32(object + 0x9Cu);
    if (override_stride != 0u) c.mem_write32(out + 4u, override_stride);
    if (c.mem_read32(out) != 0u) {
        const u32 locked = tank_object(c, tank);
        c.mem_write32(locked + 0x3Cu, c.mem_read32(locked + 0x3Cu) | 0x40000000u | attribute);
    }
}

struct Cursor {
    u32 pointer, stride;
    GEKKO_ALWAYS_INLINE u32 next() noexcept {
        const u32 at = pointer;
        pointer += stride;
        return at;
    }
};

// The note of the last served fill (native_effect_update.h, last_fill).
struct FillNote {
    u32 tank = 0;
    u32 first_unit = 0;
};
FillNote g_fill_note;
std::atomic<bool> g_identities_wanted{false};
}  // namespace

bool last_fill(u32 tank, u32* first_unit) noexcept {
    if (tank == 0u || g_fill_note.tank != tank)
        return false;
    *first_unit = g_fill_note.first_unit;
    return true;
}

void want_particle_identities(bool on) noexcept { g_identities_wanted.store(on, std::memory_order_relaxed); }
bool particle_identities_wanted() noexcept { return g_identities_wanted.load(std::memory_order_relaxed); }

bool serve_ptank_fill(Cpu& c) {
    g_fill_note.tank = 0u;
    const u32 system = c.gpr[3];
    const u32 sp0 = c.gpr[1];
    const u32 sp = sp0 - 0x60u;
    const u32 flags = c.mem_read32(system + 0x18u);
    const bool draw = (flags & 1u) != 0u && (flags & 8u) == 0u && c.mem_read32(system + 0x110u) != 0u;

    // The original's frame words (the same bytes in guest memory whichever way the call runs).
    c.mem_write32(sp, sp0);
    c.mem_write32(sp + 0x64u, c.lr);
    c.mem_write32(sp + 0x58u, c.gpr[30]);
    c.mem_write32(sp + 0x5Cu, c.gpr[31]);
    if (!draw) return true;  // the original restores r30 / r31 / r1 and returns at once: nothing else changes

    // 1. lock the five attribute streams: {pointer, stride} at sp+0x2C (positions), 0x24 (colour bytes), 0x1C (scale pairs),
    //    0x14 (rotation), 0x0C (uv rectangle)
    const u32 tank = c.mem_read32(system + 0x13Cu);
    lock_stream(c, tank, 1u, 0x4Cu, 0x50u, sp + 0x2Cu);
    lock_stream(c, tank, 2u, 0x6Cu, 0x70u, sp + 0x24u);
    lock_stream(c, tank, 4u, 0x64u, 0x68u, sp + 0x1Cu);
    lock_stream(c, tank, 0x20u, 0x7Cu, 0x80u, sp + 0x14u);
    lock_stream(c, tank, 0x80u, 0x84u, 0x88u, sp + 0x0Cu);
    Cursor position{c.mem_read32(sp + 0x2Cu), c.mem_read32(sp + 0x30u)};
    Cursor colour{c.mem_read32(sp + 0x24u), c.mem_read32(sp + 0x28u)};
    Cursor scale{c.mem_read32(sp + 0x1Cu), c.mem_read32(sp + 0x20u)};
    Cursor rotation{c.mem_read32(sp + 0x14u), c.mem_read32(sp + 0x18u)};
    Cursor uv{c.mem_read32(sp + 0x0Cu), c.mem_read32(sp + 0x10u)};

    // 2. the positions: optionally transformed by the matrix at system+0x54 (0x8040C6CC, owned natively, then the vector copy
    //    0x800091D0 through the frame's second temporary)
    const u32 head = c.mem_read32(system + 0xFCu);
    if ((c.mem_read32(system + 0x108u) & 0x800u) != 0u) {
        for (u32 unit = head; unit != 0u; unit = c.mem_read32(unit + 0x68u)) {
            c.gpr[3] = sp + 0x34u;
            c.gpr[4] = system + 0x54u;
            c.gpr[5] = unit + 4u;
            math_affine::serve_point(c);
            const u32 x = c.mem_read32(sp + 0x34u), y = c.mem_read32(sp + 0x38u), z = c.mem_read32(sp + 0x3Cu);
            c.mem_write32(sp + 0x40u, x);
            c.mem_write32(sp + 0x44u, y);
            c.mem_write32(sp + 0x48u, z);
            const u32 at = position.next();
            c.mem_write32(at, x);
            c.mem_write32(at + 4u, y);
            c.mem_write32(at + 8u, z);
        }
    } else {
        for (u32 unit = head; unit != 0u; unit = c.mem_read32(unit + 0x68u)) {
            const u32 at = position.next();
            c.mem_write32(at, c.mem_read32(unit + 4u));
            c.mem_write32(at + 4u, c.mem_read32(unit + 8u));
            c.mem_write32(at + 8u, c.mem_read32(unit + 0xCu));
        }
    }

    // 3. colour (the four 10.22 ints >> 22 as bytes), scale pair, rotation (wrapped into [-pi, pi] in place), uv rectangle
    const F pi = L(c.mem_read32(kPiConstant));
    const F negative_pi = pi ^ 0x8000000000000000ull;
    const F zero = L(c.mem_read32(kZeroFloat));
    const F two = L(c.mem_read32(kTwoConstant));
    for (u32 unit = head; unit != 0u; unit = c.mem_read32(unit + 0x68u)) {
        const u32 colour_at = colour.next();
        c.mem_write8(colour_at, static_cast<u8>(c.mem_read32(unit + 0x54u) >> 22));
        c.mem_write8(colour_at + 1u, static_cast<u8>(c.mem_read32(unit + 0x58u) >> 22));
        c.mem_write8(colour_at + 2u, static_cast<u8>(c.mem_read32(unit + 0x5Cu) >> 22));
        c.mem_write8(colour_at + 3u, static_cast<u8>(c.mem_read32(unit + 0x60u) >> 22));
        const u32 scale_at = scale.next();
        c.mem_write32(scale_at, c.mem_read32(unit + 0x28u));
        c.mem_write32(scale_at + 4u, c.mem_read32(unit + 0x2Cu));
        const u32 rotation_at = rotation.next();
        u32 turns = 0;
        for (;;) {
            const F angle = L(c.mem_read32(unit + 0x18u));
            if (greater(c, angle, pi, true) || less(c, angle, negative_pi, true)) {
                if (++turns > kWrapLimit) {  // declined (native_effect_update.h): the original runs from its first instruction
                    c.gpr[3] = system;       // (the position pass used r3..r5 for the matrix owner)
                    return false;
                }
                const F again = L(c.mem_read32(unit + 0x18u));
                const F wrapped = less(c, angle, zero, true) ? ppc::fps_madd(c, two, pi, again) : ppc::fps_nmsub(c, two, pi, again);
                c.mem_write32(unit + 0x18u, S(wrapped));
                continue;
            }
            c.mem_write32(rotation_at, S(angle));
            break;
        }
        const u32 uv_at = uv.next();
        c.mem_write32(uv_at, c.mem_read32(unit + 0x34u));
        c.mem_write32(uv_at + 4u, c.mem_read32(unit + 0x38u));
        c.mem_write32(uv_at + 8u, c.mem_read32(unit + 0x40u));
        c.mem_write32(uv_at + 0xCu, c.mem_read32(unit + 0x44u));
    }
    g_fill_note.first_unit = head;
    g_fill_note.tank = tank;
    c.mem_write32(sp + 0x2Cu, position.pointer);
    c.mem_write32(sp + 0x24u, colour.pointer);
    c.mem_write32(sp + 0x1Cu, scale.pointer);
    c.mem_write32(sp + 0x14u, rotation.pointer);
    c.mem_write32(sp + 0x0Cu, uv.pointer);

    // 4. unlock
    {
        const u32 object = tank_object(c, tank);
        c.mem_write32(object + 0x40u, c.mem_read32(object + 0x40u) | c.mem_read32(object + 0x3Cu));
        c.mem_write32(object + 0x3Cu, 0u);
    }

    // 5. the draw (render-state get / set, the tank's render callback, the render-state restore) stays the game's own code: the
    //    translated body is entered at the instruction after the unlock call, with the registers it reads (r1, r31) as the
    //    original has them there. A callee that yields behaves as it does in the original: the hand-back keeps the frame in
    //    guest memory and the dispatcher resumes inside the body.
    c.gpr[1] = sp;
    c.gpr[31] = system;
    c.gpr[30] = 0u;
    guest_continue(c, kPTankTail, c.lr & ~3u, sp0);
    return true;
}

void install(Cpu& cpu) {
    struct Entry {
        u32 address;
        void (*serve)(Cpu&);
        const char* name;
    };
    static constexpr Entry kEntries[] = {
        {kAlphaRamp, serve_alpha_ramp, "native effect command: alpha ramp"},
        {kScalar, serve_scalar, "native effect command: scalar"},
        {kVector, serve_vector, "native effect command: vector"},
        {kVector2D, serve_vector, "native effect command: 2D scale"},
        {kUnitStep, serve_unit_step, "native effect particle system: unit step"}};
    for (const Entry& e : kEntries) {
        cpu.install_hle(e.address, [serve = e.serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, e.name);
    }
    cpu.install_hle(kPTankFill, serve_ptank_fill, "native effect particle renderer: vertex fill");
}

}  // namespace gekko::effect_update
