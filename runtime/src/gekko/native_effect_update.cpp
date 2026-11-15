// The native owners of the effect system's particle update (gekko/native_effect_update.h).

#include "gekko/native_effect_update.h"

#include "gekko/cpu.h"
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
GEKKO_ALWAYS_INLINE Step prologue(Cpu& c, u32 self, F dt) noexcept {
    if (c.mem_read32(self + 0x18u) == 0u) {
        finish(c, self);
        return Step::Finished;
    }
    if (c.mem_read8(self + 0x10u) == 0u) {
        c.mem_write8(self + 0x10u, 1u);
        return Step::Alive;
    }
    c.mem_write32(self + 0x1Cu, S(ppc::fps_add(c, L(c.mem_read32(self + 0x1Cu)), dt)));
    const F elapsed = L(c.mem_read32(self + 0x1Cu));
    const F duration = L(c.mem_read32(self + 0x20u));
    if (greater(c, elapsed, duration, true)) {
        finish(c, self);
        return Step::Finished;
    }
    return Step::Run;
}

bool alpha_ramp(Cpu& c, u32 self, F dt) noexcept {
    const Step step = prologue(c, self, dt);
    if (step != Step::Run) return step == Step::Alive;
    const u32 rate = c.mem_read32(self + 0x24u);
    if (rate == 0u) return true;
    // (double)rate through the 2^52 bias, rounded to a single by fsubs, scaled by dt, truncated to an int
    const F biased = 0x4330000000000000ull | static_cast<u64>(rate ^ 0x80000000u);
    F delta = ppc::fps_sub(c, biased, c.mem_read64(kMagicDouble));
    delta = ppc::fps_mul(c, delta, dt);
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

bool scalar_update(Cpu& c, u32 self, F dt) noexcept {
    const Step step = prologue(c, self, dt);
    if (step != Step::Run) return step == Step::Alive;
    const u32 cell = c.mem_read32(self + 0x18u);
    const F rate = L(c.mem_read32(self + 0x24u));
    const F current = L(c.mem_read32(cell));
    c.mem_write32(cell, S(ppc::fps_madd(c, rate, dt, current)));
    return true;
}

// target[i] += rate[i] * dt: the rates are all read (scaled) first, then each target word is read, added and stored in turn
// (0x800091A4 into a frame temporary, then 0x8040E048).
GEKKO_ALWAYS_INLINE void add_scaled(Cpu& c, u32 target, u32 vector, F dt) noexcept {
    const F x = ppc::fps_mul(c, L(c.mem_read32(vector)), dt);
    const F y = ppc::fps_mul(c, L(c.mem_read32(vector + 4u)), dt);
    const F z = ppc::fps_mul(c, L(c.mem_read32(vector + 8u)), dt);
    c.mem_write32(target, S(ppc::fps_add(c, L(c.mem_read32(target)), x)));
    c.mem_write32(target + 4u, S(ppc::fps_add(c, L(c.mem_read32(target + 4u)), y)));
    c.mem_write32(target + 8u, S(ppc::fps_add(c, L(c.mem_read32(target + 8u)), z)));
}

bool vector_update(Cpu& c, u32 self, F dt) noexcept {
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
        case kAlphaRamp: return alpha_ramp(c, command, f.dt);
        case kScalar: return scalar_update(c, command, f.dt);
        case kVector:
        case kVector2D: return vector_update(c, command, f.dt);
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

GEKKO_ALWAYS_INLINE void scale_in_place(Cpu& c, u32 vector, F factor) noexcept {
    for (u32 lane = 0; lane < 3u; ++lane)
        c.mem_write32(vector + 4u * lane, S(ppc::fps_mul(c, L(c.mem_read32(vector + 4u * lane)), factor)));
}
}  // namespace

void serve_alpha_ramp(Cpu& c) {
    c.gpr[3] = alpha_ramp(c, c.gpr[3], c.fpr[1].bits[0]) ? 1u : 0u;
}

void serve_scalar(Cpu& c) {
    c.gpr[3] = scalar_update(c, c.gpr[3], c.fpr[1].bits[0]) ? 1u : 0u;
}

void serve_vector(Cpu& c) {
    c.gpr[3] = vector_update(c, c.gpr[3], c.fpr[1].bits[0]) ? 1u : 0u;
}

void serve_unit_step(Cpu& c) {
    Frame f{c, c.gpr[3], c.fpr[1].bits[0]};
    const u32 self = f.self;
    const F dt = f.dt;

    // 1. age the units (unless the definition says there is no limit) and flag the ones past the limit
    if ((c.mem_read32(self + 0x108u) & 0x100u) == 0u) {
        const F limit = ppc::fps_add(c, L(c.mem_read32(kTinyConstant)), L(c.mem_read32(c.mem_read32(self + 0x124u) + 0xCu)));
        for (u32 unit = c.mem_read32(self + 0xFCu); unit != 0u; unit = c.mem_read32(unit + 0x68u)) {
            if ((c.mem_read32(self + 0x18u) & 2u) != 0u)  // the system's flag, read again for every unit
                c.mem_write32(unit + 0x50u, S(ppc::fps_add(c, L(c.mem_read32(unit + 0x50u)), dt)));
            if (greater(c, L(c.mem_read32(unit + 0x50u)), limit, true))
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
    const F zero = L(c.mem_read32(kZeroConstant));
    for (u32 unit = c.mem_read32(self + 0xFCu); unit != 0u; unit = c.mem_read32(unit + 0x68u)) {
        add_scaled(c, unit + 4u, unit + 0x1Cu, dt);
        const F drag = L(c.mem_read32(self + 0x118u));
        if (!equal(c, zero, drag, false)) {
            c.gpr[3] = unit + 0x1Cu;
            vec_math::serve_length(c);  // the game's length: f1
            const F length = c.fpr[1].bits[0];
            if (greater(c, length, L(c.mem_read32(kTinyConstant)), true)) {
                const F damped = ppc::fps_mul(c, length, L(c.mem_read32(self + 0x118u)));
                if (greater(c, damped, length, true)) {
                    scale_in_place(c, unit + 0x1Cu, L(c.mem_read32(kZeroConstant)));
                } else {
                    const F kept = ppc::fps_sub(c, length, damped);
                    scale_in_place(c, unit + 0x1Cu, ppc::fps_div(c, kept, length));
                }
            }
        }
        const F gravity = L(c.mem_read32(self + 0x114u));
        if (!equal(c, zero, gravity, false)) {
            const F y = L(c.mem_read32(unit + 0x20u));
            c.mem_write32(unit + 0x20u, S(ppc::fps_nmsub(c, dt, gravity, y)));
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
}  // namespace

bool serve_ptank_fill(Cpu& c) {
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
