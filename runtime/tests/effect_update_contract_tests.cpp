// liveness-roots: reference-interpreter-only
// Original-DOL contract for the owned effect-system particle update (gekko/native_effect_update.h): the original instructions
// come from the pinned DOL and run in the reference interpreter; the native owner runs on a second machine with the same DOL
// and the same memory. Random particle systems: units with command lists in every class (the three owned update functions, the
// 2D scale alias, and a command class the owner reaches through the game's own function), shared stub functions standing in for
// the script step and the deleting destructors on BOTH machines (they log their arguments and mutate memory from a hash), so the
// calls the owner makes - which function, which arguments, in which order - are compared along with every word of the arena.
// Float inputs: zeros, denormals, the 2^-126 boundary, infinities, NaNs, huge and tiny normals, in both FPSCR[NI] modes; targets
// that alias the command's own fields. Compared: the arena bytes (including the call log), the pool global, r1 / r14..r31 and
// f14..f31, an update's r3 result, and the FPSCR control byte. Negative controls: a wrong native must be caught by the same
// comparison.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_effect_update.h"
#include "gekko/ppc_ops.h"

#include <chrono>
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
namespace eu = gekko::effect_update;

constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kArena = 0x81400000u, kArenaSize = 0x2800u;
constexpr u32 kPoolGlobal = 0x805E25C8u;
constexpr u32 kMagicDouble = 0x8051B848u;  // 0x4330000080000000 in the image
constexpr u32 kScriptStep = 0x80429224u;
constexpr u32 kDtorStub = 0x81500100u, kUpdateStub = 0x81500200u;
// the particle renderer's draw: render-state get / set and the tank's render callback (stubs on both machines)
constexpr u32 kGetStub = 0x81500300u, kSetStub = 0x81500310u, kDrawStub = 0x81500320u;
constexpr u32 kTankOffsetWord = 0x805F24F0u, kRenderStateGlobal = 0x805F265Cu;

// arena layout
constexpr u32 kLog = kArena, kLogEntries = kArena + 0x10u, kLogLimit = 0x40u;  // 16-byte entries
constexpr u32 kSpare = kArena + 0x2100u;
constexpr u32 kSystem = kArena + 0x1000u, kLimitRecord = kArena + 0x1200u, kPool = kArena + 0x1300u;
constexpr u32 kVtables = kArena + 0x1500u, kRecords = kArena + 0x1600u, kUnits = kArena + 0x1800u,
              kCommands = kArena + 0x1C00u, kTargets = kArena + 0x2200u;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

u32 g_seed = 0x0DDBA11u;
u32 rnd() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8 ^ (g_seed << 16); }
u32 pick(u32 n) { return rnd() % n; }

// The plain profile: nine fields in ten are ordinary values, so that most operations of a call have ordinary operands and
// results (the owner's single-precision route, gekko/fast_single.h) while the exceptional classes still turn up beside them.
bool g_plain = false;

u32 float_word() {  // a float for a field: mostly ordinary, with every exceptional class represented
    if (g_plain && pick(10) != 0) {
        // ordinary values over a wide range of magnitudes: sums that cancel, products that stay ordinary
        const u32 kind = pick(8);
        float v = (static_cast<float>(pick(2000001)) - 1000000.0f) / 250000.0f;
        if (kind == 0) v = static_cast<float>(static_cast<s32>(pick(2001)) - 1000) * 0.25f;  // short mantissas: exact sums, ties
        else if (kind == 1) v *= 4096.0f;
        else if (kind == 2) v *= 1.0f / 65536.0f;
        u32 w;
        std::memcpy(&w, &v, 4);
        return w;
    }
    const u32 r = pick(100);
    if (r < 62) {  // ordinary values in a useful range
        const float v = (static_cast<float>(pick(2000001)) - 1000000.0f) / 250000.0f;
        u32 w;
        std::memcpy(&w, &v, 4);
        return w;
    }
    if (r < 68) return pick(2) ? 0u : 0x80000000u;
    if (r < 72) return (pick(2) << 31) | (1u + pick(0x7FFFFFu));                       // denormal
    if (r < 75) return (pick(2) << 31) | 0x00800000u | pick(4);                         // just above the boundary
    if (r < 78) return (pick(2) << 31) | 0x7F800000u;                                   // infinity
    if (r < 82) return (pick(2) << 31) | 0x7F800000u | (1u + pick(0x7FFFFFu));          // NaN, any payload
    if (r < 85) return (pick(2) << 31) | (0x7F7FFFFFu - pick(4));                       // huge
    if (r < 90) return (pick(2) << 31) | ((0x20u + pick(0x20u)) << 23) | pick(0x800000u);  // tiny normals
    return rnd();
}
u32 small_float() {  // a time-like value, 0 .. 2
    if (pick(12) == 0) return float_word();
    const float v = static_cast<float>(pick(2001)) / 1000.0f;
    u32 w;
    std::memcpy(&w, &v, 4);
    return w;
}
u32 limit_float() {
    if (pick(12) == 0) return float_word();
    const float v = static_cast<float>(pick(3001)) / 1000.0f;
    u32 w;
    std::memcpy(&w, &v, 4);
    return w;
}
u64 dt_bits() {  // a float argument: nearly always a value a single holds
    switch (pick(g_plain ? 14 : 10)) {
        case 0: return single_to_double_bits(float_word());
        case 1: return single_to_double_bits(0u);
        case 10: {  // (plain profile) a double that no single holds: the register is a double register
            const float v = static_cast<float>(1u + pick(60000)) / 1000000.0f;
            u32 w;
            std::memcpy(&w, &v, 4);
            return single_to_double_bits(w) | (1u + pick(0x0FFFFFFFu));
        }
        default: {
            const float v = static_cast<float>(pick(60001)) / 1000000.0f;
            u32 w;
            std::memcpy(&w, &v, 4);
            return single_to_double_bits(w);
        }
    }
}

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    explicit Machine(const std::vector<u8>& dol, bool script_stub = true) {
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
        install_stubs(script_stub);
    }
    // The pool object's word the unit step swaps in around its passes: the stubs log it, so its value at each call is compared.
    static u32 pool_word(Cpu& c) { return c.mem_read32(c.mem_read32(kPoolGlobal) + 4u); }
    static void log(Cpu& c, u32 tag, u32 a, u32 b, u32 d) {
        const u32 count = c.mem_read32(kLog);
        if (count < kLogLimit) {
            const u32 at = kLogEntries + 16u * count;
            c.mem_write32(at, tag);
            c.mem_write32(at + 4u, a);
            c.mem_write32(at + 8u, b);
            c.mem_write32(at + 12u, d);
        }
        c.mem_write32(kLog, count + 1u);
    }
    static u32 mix(u32 a, u32 b) { return (a * 0x9E3779B1u) ^ (b + 0x7F4A7C15u + (a << 6)); }
    void install_stubs(bool script_stub) {
        // the script step: logs, then finishes the unit, clears its cursor, or does nothing (from a hash of the unit and the log)
        if (script_stub) cpu.install_hle(kScriptStep, [](Cpu& c) {
            const u32 unit = c.gpr[3];
            if (c.mem_read32(unit + 0x6Cu) == 0u) return true;  // the original does nothing without a script cursor (see Units without scripts)
            const u32 h = mix(unit, c.mem_read32(kLog));
            log(c, 1u, unit, 0u, pool_word(c));
            switch (h % 4u) {
                case 0: c.mem_write32(unit + 0x4Cu, c.mem_read32(unit + 0x4Cu) | 1u); break;
                case 1: c.mem_write32(unit + 0x6Cu, 0u); break;
                default: break;
            }
            return true;
        }, "stub script step");
        cpu.install_hle(kDtorStub, [](Cpu& c) {
            const u32 object = c.gpr[3];
            log(c, 2u, object, c.gpr[4], pool_word(c));
            c.mem_write32(object + 0x28u, 0xDEAD0000u | (c.mem_read32(kLog) & 0xFFFFu));
            return true;
        }, "stub destructor");
        cpu.install_hle(kUpdateStub, [](Cpu& c) {
            const u32 object = c.gpr[3];
            log(c, 3u, object, static_cast<u32>(c.fpr[1].bits[0] >> 32), static_cast<u32>(c.fpr[1].bits[0]) ^ (pool_word(c) * 0x9E3779B1u));
            const u32 h = mix(object, c.mem_read32(kLog));
            c.mem_write32(object + 0x1Cu, c.mem_read32(object + 0x1Cu) + 1u);
            c.gpr[3] = (h & 3u) == 0u ? 0x100u : ((h & 3u) == 1u ? 0u : 0x5A01u);  // low byte 0 = finished
            return true;
        }, "stub command update");
        // the renderer's draw: each logs what the memory looks like at that moment, so the order of the fill's effects is compared
        cpu.install_hle(kGetStub, [](Cpu& c) {
            log(c, 11u, c.gpr[3], c.gpr[4], c.mem_read32(c.gpr[4]));
            c.mem_write32(c.gpr[4], 0x55AA0000u | (c.mem_read32(kLog) & 0xFFFFu));
            return true;
        }, "stub render state get");
        cpu.install_hle(kSetStub, [](Cpu& c) {
            log(c, 12u, c.gpr[3], c.gpr[4], c.mem_read32(c.gpr[1] + 0x68u));
            return true;
        }, "stub render state set");
        cpu.install_hle(kDrawStub, [](Cpu& c) {
            const u32 tank = c.gpr[3];
            const u32 object = c.mem_read32(tank + c.mem_read32(kTankOffsetWord));
            log(c, 13u, tank, c.mem_read32(object + 0x3Cu) ^ (c.mem_read32(object + 0x40u) * 3u), c.mem_read32(object + 4u));
            c.mem_write32(object + 0x44u, c.mem_read32(object + 0x44u) + 1u);
            return true;
        }, "stub tank render callback");
    }
    void prepare(u32 entry, bool ni) {
        cpu.reset();
        cpu.set_msr(cpu.msr() | kMsrFP);
        cpu.fpscr = ni ? kFpscrNI : 0u;
        cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = kReturn;
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13;
    }
    void run_original(unsigned bound = 2000000u) {
        unsigned steps = 0;
        u32 ring[24] = {};
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < bound) {
            ring[steps % 24u] = cpu.pc;
            cpu.run_steps(1);
        }
        if (!(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack)) {
            std::fprintf(stderr, "original run ended: pc %08X r1 %08X steps %u stopped %d reason %d at %08X: %s\n", cpu.pc, cpu.gpr[1], steps,
                         cpu.stopped() ? 1 : 0, static_cast<int>(cpu.stop().reason), cpu.stop().address, cpu.stop().detail.c_str());
            for (u32 k = 1; k <= 24u; ++k) std::fprintf(stderr, " %08X", ring[(steps + k) % 24u]);
            std::fprintf(stderr, "%c", 10);
            throw std::runtime_error("original call failed return/stack contract");
        }
    }
};

// Writes one word to both machines.
struct Pair {
    Machine& a;
    Machine& b;
    void w(u32 at, u32 v) { a.memory.write32(at, v); b.memory.write32(at, v); }
};

// What kind of call a case makes.
enum class Kind { Alpha, Scalar, Vector, Vector2D, Units };
struct Case {
    const char* name;
    u32 entry;
    Kind kind;
    void (*serve)(Cpu&);
    bool scripts = true;  // false: every unit has no script cursor, and the original's own script step runs (no stub)
};

constexpr u32 kAlphaMax = 0x3FC00000u, kAlphaMin = 0x3FFFFFu;

// One command object at `at`: class, target and timing.
void build_command(Pair& p, u32 at, u32 unit, Kind kind, u32 target_slot) {
    for (u32 off = 0; off < 0x30u; off += 4u) p.w(at + off, float_word());
    // vtable
    u32 which = pick(5);
    if (kind != Kind::Units) which = static_cast<u32>(kind);
    p.w(at, kVtables + 0x10u * which);
    p.w(at + 4u, kRecords + 0x10u * pick(8));
    p.w(at + 8u, 0u);   // links are set by the caller
    p.w(at + 0xCu, 0u);
    p.w(at + 0x10u, pick(3) == 0 ? 0u : (pick(2) ? 0x01000000u : 0x7F000000u));  // started byte (the high byte of the word)
    p.w(at + 0x14u, pick(10) == 0 ? kSpare : unit + 0x4Cu);               // the unit's flags word (or a spare)
    u32 target = kTargets + 0x10u * target_slot;
    const u32 alias = pick(12);
    if (alias == 0) target = at + 0x24u;
    else if (alias == 1) target = at + 0x1Cu;
    else if (alias == 2) target = at + 0x18u;
    else if (alias == 3) target = unit + 0x4Cu;
    p.w(at + 0x18u, pick(8) == 0 ? 0u : target);
    p.w(at + 0x1Cu, small_float());
    p.w(at + 0x20u, small_float());
    if (which == 0) {  // the alpha ramp: an int rate, an int target around the clamps
        const u32 r = pick(6);
        p.w(at + 0x24u, r == 0 ? 0u : r == 1 ? rnd() : r == 2 ? 0x7FFFFFFFu - pick(3) : r == 3 ? 0x80000000u + pick(3) : pick(0x20000000u) * (pick(2) ? 1u : 0xFFFFFFFFu));
        const u32 t = pick(6);
        const u32 value = t == 0 ? kAlphaMax - pick(0x100000) : t == 1 ? kAlphaMin + pick(0x100000) : t == 2 ? kAlphaMax + pick(3) - 1u
                          : t == 3 ? kAlphaMin + pick(3) - 1u : t == 4 ? rnd() : kAlphaMin + pick(kAlphaMax - kAlphaMin);
        if (target >= kTargets && target < kTargets + 0x200u) p.w(target, value);
    }
}

void build_system(Pair& p, Kind kind, bool scripts) {
    // system
    for (u32 off = 0; off < 0x140u; off += 4u) p.w(kSystem + off, float_word());
    p.w(kSystem + 0x18u, pick(2) ? 2u | pick(16) << 2 : pick(16) << 2);
    p.w(kSystem + 0x108u, pick(3) == 0 ? 0x100u | pick(0x80) : pick(0x80));
    p.w(kSystem + 0x114u, pick(3) == 0 ? 0u : float_word());  // gravity
    p.w(kSystem + 0x118u, pick(3) == 0 ? 0u : (pick(2) ? small_float() : float_word()));  // drag
    p.w(kSystem + 0x124u, kLimitRecord);
    p.w(kLimitRecord + 0xCu, limit_float());
    p.w(kSystem + 0x134u, rnd());
    p.w(kSystem + 0x138u, rnd());
    p.w(kPoolGlobal, kPool);
    p.w(kPool + 4u, rnd());
    // The int -> double bias of the alpha ramp is a constant the original loads from memory on every call: mostly the
    // game's own value, now and then another one (an owner that assumes the value is caught).
    const u32 bias = pick(16);
    p.w(kMagicDouble, bias == 0 ? 0x43300000u : bias == 1 ? 0x43300001u : 0x43300000u);
    p.w(kMagicDouble + 4u, bias == 0 ? 0u : bias == 1 ? rnd() : 0x80000000u);
    // vtables: [0] [4] destructor [0xC] update
    const u32 updates[5] = {eu::kAlphaRamp, eu::kScalar, eu::kVector, eu::kVector2D, kUpdateStub};
    for (u32 v = 0; v < 5u; ++v) {
        p.w(kVtables + 0x10u * v, 0u);
        p.w(kVtables + 0x10u * v + 4u, 0u);
        p.w(kVtables + 0x10u * v + 8u, kDtorStub);
        p.w(kVtables + 0x10u * v + 0xCu, updates[v]);
    }
    for (u32 r = 0; r < 8u; ++r) {
        p.w(kRecords + 0x10u * r, (rnd() & ~0xFF00u) | (pick(2) ? (1u + pick(255)) << 8 : 0u));  // byte +2: nonzero or zero
        p.w(kRecords + 0x10u * r + 4u, rnd());
        p.w(kRecords + 0x10u * r + 8u, rnd());
    }
    for (u32 off = 0; off < 0x200u; off += 4u) p.w(kTargets + off, float_word());
    p.w(kSpare, 0u);
    // units
    const u32 count = kind == Kind::Units ? pick(6) : 1u;
    u32 prev = 0, first = 0;
    u32 command_at = kCommands;
    u32 target_slot = 0;
    for (u32 i = 0; i < count; ++i) {
        const u32 unit = kUnits + 0x80u * i;
        for (u32 off = 0; off < 0x7Cu; off += 4u) p.w(unit + off, float_word());
        p.w(unit, kVtables);  // a unit is a vtable object too: the loop that removes finished units calls its deleting destructor
        p.w(unit + 0x18u, rnd());
        u32 flags = pick(4) == 0 ? 1u : 0u;
        if (pick(2)) flags |= 2u << pick(4);
        p.w(unit + 0x4Cu, flags);
        p.w(unit + 0x50u, pick(4) == 0 ? float_word() : small_float());
        p.w(unit + 0x64u, prev);
        p.w(unit + 0x68u, 0u);
        if (prev) p.w(prev + 0x68u, unit);
        else first = unit;
        p.w(unit + 0x6Cu, scripts && pick(3) == 0 ? kRecords : 0u);
        // commands
        const u32 n = kind == Kind::Units ? pick(5) : 0u;
        u32 cprev = 0, chead = 0;
        for (u32 k = 0; k < n; ++k) {
            const u32 cmd = command_at;
            command_at += 0x30u;
            build_command(p, cmd, unit, kind, target_slot++ & 0xFu);
            p.w(cmd + 8u, cprev);
            p.w(cmd + 0xCu, 0u);
            if (cprev) p.w(cprev + 0xCu, cmd);
            else chead = cmd;
            cprev = cmd;
        }
        p.w(unit + 0x70u, chead);
        p.w(unit + 0x74u, cprev);
        prev = unit;
    }
    p.w(kSystem + 0xFCu, first);
    p.w(kSystem + 0x100u, prev);
    p.w(kSystem + 0x110u, count);
    p.w(kLog, 0u);
    // the standalone command updates take one command
    if (kind != Kind::Units) {
        build_command(p, kCommands, kUnits, kind, 0u);
    }
}

int g_failures = 0, g_checked = 0;
void report(const char* what, const std::string& detail) {
    ++g_failures;
    if (g_failures < 24) std::fprintf(stderr, "FAIL %s: %s\n", what, detail.c_str());
}

void run_case(Machine& o, Machine& n, const Case& c, unsigned calls, const std::function<void(Cpu&)>& native,
              bool expect_clean, unsigned* mismatches) {
    for (unsigned i = 0; i < calls; ++i) {
        const bool ni = (i & 1u) != 0u;
        for (u32 off = 0; off < kArenaSize; off += 4u) { o.memory.write32(kArena + off, 0u); n.memory.write32(kArena + off, 0u); }
        Pair p{o, n};
        build_system(p, c.kind, c.scripts);
        const u64 dt = dt_bits();
        const u32 self = c.kind == Kind::Units ? kSystem : kCommands;
        o.prepare(c.entry, ni);
        n.prepare(c.entry, ni);
        // callee-saved registers carry recognisable values
        const u32 gseed = rnd();
        const u64 fseed = (u64(rnd()) << 32) | rnd();
        for (Machine* m : {&o, &n}) {
            m->cpu.gpr[3] = self;
            m->cpu.fpr[1].bits[0] = dt;
            for (u32 r = 14; r < 32u; ++r) m->cpu.gpr[r] = gseed + r * 0x01010101u;
            for (u32 r = 14; r < 32u; ++r) { m->cpu.fpr[r].bits[0] = fseed ^ r; m->cpu.fpr[r].bits[1] = single_to_double_bits(0x3F000000u + ((static_cast<u32>(fseed >> 7) + r * 0x00010001u) & 0x3FFFFFu)); }
        }
        try {
            o.run_original();
        } catch (const std::exception&) {
            std::fprintf(stderr, "case %s call %u ni=%d dt=%016llX; system: head %08X count %u flags %08X; units:\n", c.name, i, ni,
                         static_cast<unsigned long long>(dt), o.memory.peek32(kSystem + 0xFC), o.memory.peek32(kSystem + 0x110),
                         o.memory.peek32(kSystem + 0x18));
            for (u32 u = o.memory.peek32(kSystem + 0xFC); u != 0u && u < 0x82000000u; u = o.memory.peek32(u + 0x68))
                std::fprintf(stderr, "  unit %08X flags %08X cursor %08X cmds %08X..%08X\n", u, o.memory.peek32(u + 0x4C), o.memory.peek32(u + 0x6C),
                             o.memory.peek32(u + 0x70), o.memory.peek32(u + 0x74));
            throw;
        }
        native(n.cpu);
        bool same = true;
        std::string detail;
        char t[256];
        for (u32 off = 0; off < kArenaSize && same; off += 4u)
            if (o.memory.peek32(kArena + off) != n.memory.peek32(kArena + off)) {
                same = false;
                std::snprintf(t, sizeof t, "%s ni=%d at arena+0x%04X: original %08X native %08X (dt %016llX call %u)", c.name, ni, off,
                              o.memory.peek32(kArena + off), n.memory.peek32(kArena + off), static_cast<unsigned long long>(dt), i);
                detail = t;
            }
        if (!same && expect_clean && g_failures < 3) {
            for (Machine* m : {&o, &n}) {
                std::fprintf(stderr, "%s log (%u entries):", m == &o ? "original" : "native", m->memory.peek32(kLog));
                for (u32 e = 0; e < m->memory.peek32(kLog) && e < 24u; ++e)
                    std::fprintf(stderr, " [%u %08X %08X]", m->memory.peek32(kLogEntries + 16u * e), m->memory.peek32(kLogEntries + 16u * e + 4u),
                                 m->memory.peek32(kLogEntries + 16u * e + 8u));
                std::fprintf(stderr, "%c", 10);
            }
        }
        if (same && (o.memory.peek32(kPoolGlobal) != n.memory.peek32(kPoolGlobal))) {
            same = false;
            detail = std::string(c.name) + ": the pool global differs";
        }
        if (same && c.kind != Kind::Units && o.cpu.gpr[3] != n.cpu.gpr[3]) {
            same = false;
            std::snprintf(t, sizeof t, "%s ni=%d r3: original %08X native %08X", c.name, ni, o.cpu.gpr[3], n.cpu.gpr[3]);
            detail = t;
        }
        if (same && n.cpu.gpr[1] != kStack) { same = false; detail = std::string(c.name) + ": r1 changed"; }
        for (u32 r = 14; r < 32u && same; ++r)
            if (o.cpu.gpr[r] != n.cpu.gpr[r] || o.cpu.fpr[r].bits[0] != n.cpu.fpr[r].bits[0] || o.cpu.fpr[r].bits[1] != n.cpu.fpr[r].bits[1]) {
                same = false;
                std::snprintf(t, sizeof t, "%s: a callee-saved register r/f%u differs", c.name, r);
                detail = t;
            }
        // FPSCR: everything but FR / FI / FPRF (the result flags the last compare or operation leaves, which no code reads):
        // the control byte and the sticky exception bits, which an unordered compare raises.
        if (same && (o.cpu.fpscr & ~0x7F000u) != (n.cpu.fpscr & ~0x7F000u)) {
            same = false;
            std::snprintf(t, sizeof t, "%s ni=%d FPSCR differs: original %08X native %08X (call %u)", c.name, ni, o.cpu.fpscr, n.cpu.fpscr, i);
            detail = t;
        }
        if (same && n.cpu.stopped()) { same = false; detail = std::string(c.name) + ": the native machine stopped"; }
        ++g_checked;
        if (!same) {
            if (mismatches) ++*mismatches;
            if (expect_clean) report(c.name, detail);
        }
    }
}

// ---- the particle renderer's vertex fill (0x80434824) ----------------------------------------------------------------------

constexpr u32 kPT = 0x81410000u, kPTSize = 0x2000u;
constexpr u32 kPtSystem = kPT, kPtTank = kPT + 0x200u, kPtObject = kPT + 0x400u, kPtRenderState = kPT + 0x600u,
              kPtStreams = kPT + 0x800u, kPtUnits = kPT + 0x1800u;
constexpr u32 kStackWindow = 0x100u;

u32 rotation_word() {  // a rotation angle: around the wrap boundaries, plain, and the classes the compare must treat as the original does
    switch (pick(14)) {
        case 0: return 0u;
        case 1: return 0x40490FDBu + pick(5) - 2u;           // pi and its neighbours
        case 2: return 0xC0490FDBu + pick(5) - 2u;           // -pi and its neighbours
        case 3: return 0x7FC00000u | pick(0x400000u);        // NaN
        case 4: return (pick(2) << 31) | (1u + pick(0x7FFFFFu));  // denormal
        case 5: return pick(2) ? 0x469C4000u : 0xC69C4000u;  // +-20000: about 3200 wraps
        case 6: return 0x80000000u;
        default: {
            const float v = (static_cast<float>(pick(2000001)) - 1000000.0f) / 50000.0f;  // -20 .. 20
            u32 w;
            std::memcpy(&w, &v, 4);
            return w;
        }
    }
}

void build_ptank(Pair& p, bool beyond_limit) {
    for (u32 off = 0; off < kPTSize; off += 4u) p.w(kPT + off, 0u);
    for (u32 off = 0; off < 0x140u; off += 4u) p.w(kPtSystem + off, float_word());
    const u32 units = beyond_limit ? 1u + pick(5) : pick(6);
    u32 flags = pick(8) == 0 ? 0u : 1u;
    if (pick(8) == 0) flags |= 8u;
    if (beyond_limit) flags = (flags | 1u) & ~8u;
    flags |= pick(2) ? 2u : 0u;
    flags |= pick(16) << 4;
    p.w(kPtSystem + 0x18u, flags);
    p.w(kPtSystem + 0x108u, (pick(2) ? 0x800u : 0u) | (pick(2) ? 2u : 0u) | pick(0x100));
    p.w(kPtSystem + 0x110u, beyond_limit ? units : pick(12) == 0 ? 0u : units + (pick(6) == 0 ? 3u : 0u));
    p.w(kPtSystem + 0x13Cu, kPtTank);
    p.w(kPtSystem + 0x144u, pick(5) == 0 ? 1u : 0u);
    for (u32 m = 0; m < 12u; ++m) p.w(kPtSystem + 0x54u + 4u * m, pick(3) == 0 ? float_word() : small_float());
    // the tank: [0x48] the render callback; the attribute table object sits at the offset the small-data word names
    p.w(kPtTank + 0x48u, kDrawStub);
    p.w(kPtTank + 0x80u, kPtObject);
    p.w(kTankOffsetWord, 0x80u);
    p.w(kRenderStateGlobal, kPtRenderState);
    p.w(kPtRenderState + 0x20u, kSetStub);
    p.w(kPtRenderState + 0x24u, kGetStub);
    // the attribute object: present attributes, stream pointers and strides, an optional stride override, the lock words
    const u32 present[5] = {1u, 2u, 4u, 0x20u, 0x80u};
    const u32 pointer_at[5] = {0x4Cu, 0x6Cu, 0x64u, 0x7Cu, 0x84u};
    u32 mask = 0;
    for (u32 i = 0; i < 5u; ++i)
        if (pick(10) != 0) mask |= present[i];
    if (pick(3) == 0) mask |= 0x100u << pick(4);  // attributes the fill does not lock
    p.w(kPtObject + 0xA0u, mask);
    for (u32 i = 0; i < 5u; ++i) {
        const u32 stream = pick(14) == 0 ? 0u : kPtStreams + 0x200u * i + 4u * pick(4);
        p.w(kPtObject + pointer_at[i], stream);
        p.w(kPtObject + pointer_at[i] + 4u, 4u * pick(12));
    }
    p.w(kPtObject + 0x9Cu, pick(4) == 0 ? 4u * (1u + pick(11)) : 0u);
    p.w(kPtObject + 0x3Cu, pick(3) == 0 ? rnd() & 0xFFFFu : 0u);
    p.w(kPtObject + 0x40u, pick(3) == 0 ? rnd() : 0u);
    p.w(kPtObject + 4u, rnd());
    // units
    u32 prev = 0, first = 0;
    for (u32 i = 0; i < units; ++i) {
        const u32 unit = kPtUnits + 0x80u * i;
        for (u32 off = 0; off < 0x7Cu; off += 4u) p.w(unit + off, pick(4) == 0 ? float_word() : small_float());
        p.w(unit + 0x18u, rotation_word());
        for (u32 off = 0x54u; off <= 0x60u; off += 4u) p.w(unit + off, rnd());
        p.w(unit + 0x64u, prev);
        p.w(unit + 0x68u, 0u);
        if (prev) p.w(prev + 0x68u, unit);
        else first = unit;
        prev = unit;
    }
    p.w(kPtSystem + 0xFCu, first);
    p.w(kPtSystem + 0x100u, prev);
    if (beyond_limit && first) p.w(first + 0x18u, pick(2) ? 0x49742400u : 0xC9742400u);  // +-1000000: beyond the wrap limit
    p.w(kLog, 0u);
}

// One call of the renderer fill on both machines. `native` runs on n with the same registers the original starts with.
// Compared: the arena, the whole particle region, the frame window under the stack, the first words of memory (a missing
// attribute writes there), r1, pc, r14..r31, f14..f31, the FPSCR bits other than FR / FI / FPRF (compares write those).
void run_ptank_case(Machine& o, Machine& n, unsigned calls, const std::function<bool(Cpu&)>& native, bool expect_clean,
                    unsigned* mismatches, bool declines = false, unsigned* declined = nullptr) {
    for (unsigned i = 0; i < calls; ++i) {
        const bool ni = (i & 1u) != 0u;
        for (u32 off = 0; off < kArenaSize; off += 4u) { o.memory.write32(kArena + off, 0u); n.memory.write32(kArena + off, 0u); }
        for (u32 off = 0; off < kStackWindow; off += 4u) {  // identical stack garbage on both machines
            const u32 w = rnd();
            o.memory.write32(kStack - kStackWindow + off, w);
            n.memory.write32(kStack - kStackWindow + off, w);
        }
        for (u32 off = 0; off < 0x40u; off += 4u) { o.memory.write32(off, 0xA5A50000u + off); n.memory.write32(off, 0xA5A50000u + off); }
        Pair p{o, n};
        build_ptank(p, declines);
        o.prepare(0x80434824u, ni);
        n.prepare(0x80434824u, ni);
        const u32 gseed = rnd();
        const u64 fseed = (u64(rnd()) << 32) | rnd();
        for (Machine* m : {&o, &n}) {
            m->cpu.gpr[3] = kPtSystem;
            for (u32 r = 14; r < 32u; ++r) m->cpu.gpr[r] = gseed + r * 0x01010101u;
            for (u32 r = 14; r < 32u; ++r) { m->cpu.fpr[r].bits[0] = fseed ^ r; m->cpu.fpr[r].bits[1] = single_to_double_bits(0x3F000000u + ((static_cast<u32>(fseed >> 7) + r * 0x00010001u) & 0x3FFFFFu)); }
        }
        o.run_original(declines ? 40000000u : 2000000u);
        const bool served = native(n.cpu);
        if (served && !n.cpu.stopped()) n.cpu.pc = n.cpu.lr;  // the hook path's rule (Cpu::try_hle_slow): a served call resumes at LR
        if (declines) {  // the owner declined: the original body then runs on the native machine, as the hook path does
            if (served) { report("PTank decline", "the owner served a rotation beyond the wrap limit"); continue; }
            if (n.cpu.gpr[3] != kPtSystem || n.cpu.gpr[1] != kStack) { report("PTank decline", "registers not restored on decline"); continue; }
            if (declined) ++*declined;
            n.cpu.pc = 0x80434824u; n.cpu.npc = 0x80434828u; n.cpu.lr = kReturn;
            n.run_original(40000000u);
        } else if (!served) {
            report("PTank", "declined an ordinary call");
            continue;
        }
        if (expect_clean && n.cpu.stopped() && g_failures < 3) std::fprintf(stderr, "native stopped: %s (pc %08X)\n", n.cpu.stop().detail.c_str(), n.cpu.pc);
        else if (expect_clean && g_failures < 3) std::fprintf(stderr, "native pc %08X r1 %08X exiting %d\n", n.cpu.pc, n.cpu.gpr[1], n.cpu.exiting() ? 1 : 0);
        bool same = true;
        std::string detail;
        char t[256];
        const auto word_check = [&](u32 base, u32 size, const char* what) {
            for (u32 off = 0; off < size && same; off += 4u)
                if (o.memory.peek32(base + off) != n.memory.peek32(base + off)) {
                    same = false;
                    std::snprintf(t, sizeof t, "PTank ni=%d %s +0x%04X: original %08X native %08X (call %u)", ni, what, off,
                                  o.memory.peek32(base + off), n.memory.peek32(base + off), i);
                    detail = t;
                }
        };
        word_check(kArena, kArenaSize, "arena");
        word_check(kPT, kPTSize, "particle region");
        word_check(kStack - kStackWindow, kStackWindow, "stack");
        word_check(0u, 0x40u, "low memory");
        if (same && n.cpu.gpr[1] != kStack) { same = false; detail = "PTank: r1 changed"; }
        if (same && n.cpu.pc != kReturn) { same = false; detail = "PTank: pc is not the return address"; }
        for (u32 r = 14; r < 32u && same; ++r)
            if (o.cpu.gpr[r] != n.cpu.gpr[r] || o.cpu.fpr[r].bits[0] != n.cpu.fpr[r].bits[0] || o.cpu.fpr[r].bits[1] != n.cpu.fpr[r].bits[1]) {
                same = false;
                std::snprintf(t, sizeof t, "PTank: a callee-saved register r/f%u differs", r);
                detail = t;
            }
        if (same && (o.cpu.fpscr & ~0x7F000u) != (n.cpu.fpscr & ~0x7F000u)) {
            same = false;
            std::snprintf(t, sizeof t, "PTank: FPSCR differs: original %08X native %08X", o.cpu.fpscr, n.cpu.fpscr);
            detail = t;
        }
        if (same && n.cpu.stopped()) { same = false; detail = "PTank: the native machine stopped"; }
        ++g_checked;
        if (!same) {
            if (mismatches) ++*mismatches;
            if (expect_clean) report("PTank", detail);
        }
    }
}

std::vector<u8> read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2 || argc == 3, "usage: effect_update_contract_tests main.dol [bench]");
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        Machine original(dol), native(dol), original_real(dol, false), native_real(dol, false);
        if (argc == 3) {  // timing of the hot owners on a fixed, cache-resident state (not a gate: for orientation only)
            struct Row { const char* name; Kind kind; u32 entry; void (*serve)(Cpu&); };
            for (const Row& r : {Row{"AlphaRamp", Kind::Alpha, eu::kAlphaRamp, eu::serve_alpha_ramp}, Row{"Scalar", Kind::Scalar, eu::kScalar, eu::serve_scalar},
                                 Row{"Vector", Kind::Vector, eu::kVector, eu::serve_vector}}) {
                g_seed = 7u;
                Pair p{original, native};
                for (u32 off = 0; off < kArenaSize; off += 4u) p.w(kArena + off, 0u);
                build_system(p, r.kind, true);
                p.w(kCommands + 0x10u, 0x01000000u);
                p.w(kCommands + 0x1Cu, 0u);
                p.w(kCommands + 0x20u, 0x7F7FFFFFu);
                p.w(kCommands + 0x18u, kTargets);
                p.w(kCommands + 0x24u, 0x3C23D70Au);
                p.w(kCommands + 0x28u, 0x3C23D70Au);
                p.w(kCommands + 0x2Cu, 0x3C23D70Au);
                p.w(kTargets, 0x3F800000u);
                native.prepare(r.entry, true);
                const u64 dt = single_to_double_bits(0x3C888889u);
                constexpr unsigned kN = 5000000;
                const auto t0 = std::chrono::steady_clock::now();
                for (unsigned i = 0; i < kN; ++i) {
                    native.cpu.gpr[3] = kCommands;
                    native.cpu.fpr[1].bits[0] = dt;
                    r.serve(native.cpu);
                }
                const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / kN;
                std::printf("%-10s native owner, hot cache: %.1f ns per call\n", r.name, ns);
            }
            return 0;
        }

        const Case cases[] = {
            {"AlphaRamp", eu::kAlphaRamp, Kind::Alpha, eu::serve_alpha_ramp},
            {"Scalar", eu::kScalar, Kind::Scalar, eu::serve_scalar},
            {"Vector", eu::kVector, Kind::Vector, eu::serve_vector},
            {"Vector2D", eu::kVector2D, Kind::Vector2D, eu::serve_vector},
            {"UnitStep", eu::kUnitStep, Kind::Units, eu::serve_unit_step},
            {"UnitStepNoScripts", eu::kUnitStep, Kind::Units, eu::serve_unit_step, false},
        };
        constexpr unsigned kCommandCalls = 60000, kUnitCalls = 20000;
        for (const Case& c : cases) {
            Machine& o = c.scripts ? original : original_real;
            Machine& n = c.scripts ? native : native_real;
            run_case(o, n, c, c.kind == Kind::Units ? kUnitCalls : kCommandCalls, c.serve, true, nullptr);
        }
        // The same cases on the plain profile (the owner's single-precision route), three times as many calls.
        g_plain = true;
        for (const Case& c : cases) {
            Machine& o = c.scripts ? original : original_real;
            Machine& n = c.scripts ? native : native_real;
            run_case(o, n, c, 3u * (c.kind == Kind::Units ? kUnitCalls : kCommandCalls), c.serve, true, nullptr);
        }
        const int plain_failures = g_failures;
        g_plain = false;
        if (argc == 2 && std::getenv("EFFECT_UPDATE_PLAIN_ONLY") != nullptr) {  // for the must-fail copies of the owner
            if (plain_failures != 0) {
                std::fprintf(stderr, "FAIL effect update contract: %d mismatches of %d calls\n", g_failures, g_checked);
                return 1;
            }
            std::printf("PASS effect update contract (plain profile only): %d calls compared\n", g_checked);
            return 0;
        }

        // Negative controls: the same comparison must catch a wrong native.
        unsigned caught_unfused = 0, caught_ni = 0, caught_dt = 0, caught_alpha = 0;
        run_case(original, native, cases[4], 8000, [](Cpu& cpu) {  // the wrong FPSCR[NI] mode
            cpu.fpscr ^= kFpscrNI;
            eu::serve_unit_step(cpu);
            cpu.fpscr ^= kFpscrNI;  // restored: only the arithmetic done in the wrong mode is compared
        }, false, &caught_ni);
        require(caught_ni > 0, "negative control: the wrong FPSCR[NI] mode was not caught");
        run_case(original, native, cases[4], 8000, [](Cpu& cpu) {  // one ulp of dt too much
            cpu.fpr[1].bits[0] += 0x20000000ull;
            eu::serve_unit_step(cpu);
        }, false, &caught_dt);
        require(caught_dt > 0, "negative control: a wrong time step was not caught");
        run_case(original, native, cases[0], 8000, [](Cpu& cpu) {  // a perturbed alpha result
            const u32 self = cpu.gpr[3];
            const u32 target = cpu.mem_read32(self + 0x18u);
            const u32 before = target != 0u ? cpu.mem_read32(target) : 0u;
            eu::serve_alpha_ramp(cpu);
            if (target != 0u && cpu.mem_read32(self + 0x18u) == target) cpu.mem_write32(target, cpu.mem_read32(target) ^ (before & 1u));
        }, false, &caught_alpha);
        require(caught_alpha > 0, "negative control: a perturbed alpha result was not caught");
        run_case(original, native, cases[1], 8000, [](Cpu& cpu) {  // the multiply and the add rounded separately
            const u32 self = cpu.gpr[3];
            const u64 dt = cpu.fpr[1].bits[0];
            const u32 target = cpu.mem_read32(self + 0x18u);
            const u32 before = target != 0u ? cpu.mem_read32(target) : 0u;
            const u32 started = cpu.mem_read8(self + 0x10u);
            eu::serve_scalar(cpu);
            // after a full update (started, target kept and written), redo the store with two roundings
            if (target != 0u && started != 0u && cpu.gpr[3] == 1u && cpu.mem_read32(self + 0x18u) == target &&
                cpu.mem_read32(target) != before) {
                const float rate = bits_to_float(cpu.mem_read32(self + 0x24u)), now = bits_to_float(before);
                const float step = static_cast<float>(static_cast<double>(rate) * bits_to_double(dt));
                cpu.mem_write32(target, float_to_bits(now + step));
            }
        }, false, &caught_unfused);
        require(caught_unfused > 0, "negative control: an unfused scalar update was not caught");

        // The particle renderer's vertex fill: the original body in the interpreter vs the owner + the original tail.
        constexpr unsigned kPtankCalls = 30000;
        const auto ptank = [](Cpu& cpu) { return eu::serve_ptank_fill(cpu); };
        run_ptank_case(original, native, kPtankCalls, ptank, true, nullptr);
        unsigned ptank_declined = 0;
        run_ptank_case(original, native, 12, ptank, true, nullptr, true, &ptank_declined);
        require(ptank_declined == 12, "the wrap-limit decline was not exercised");
        unsigned caught_stream = 0, caught_order = 0, caught_wrap = 0, caught_lock = 0, caught_tail = 0;
        run_ptank_case(original, native, 6000, [](Cpu& cpu) {  // a stream word flipped
            const bool served = eu::serve_ptank_fill(cpu);
            cpu.mem_write32(kPtStreams, cpu.mem_read32(kPtStreams) ^ 0x100u);
            return served;
        }, false, &caught_stream);
        require(caught_stream > 0, "negative control: a corrupted stream was not caught");
        run_ptank_case(original, native, 6000, [](Cpu& cpu) {  // the unit rotation left unwrapped
            const u32 head = cpu.mem_read32(kPtSystem + 0xFCu);
            const u32 angle = head != 0u ? cpu.mem_read32(head + 0x18u) : 0u;
            const bool served = eu::serve_ptank_fill(cpu);
            if (head != 0u && cpu.mem_read32(head + 0x18u) != angle) cpu.mem_write32(head + 0x18u, angle);
            return served;
        }, false, &caught_wrap);
        require(caught_wrap > 0, "negative control: an unwrapped rotation was not caught");
        run_ptank_case(original, native, 6000, [](Cpu& cpu) {  // the attribute unlock skipped
            const bool served = eu::serve_ptank_fill(cpu);
            cpu.mem_write32(kPtObject + 0x3Cu, cpu.mem_read32(kPtObject + 0x3Cu) | 0x40000000u);
            return served;
        }, false, &caught_lock);
        require(caught_lock > 0, "negative control: a skipped unlock was not caught");
        run_ptank_case(original, native, 6000, [](Cpu& cpu) {  // the draw tail not run: the call returns after the unlock
            const u32 lr = cpu.lr, sp0 = cpu.gpr[1], system = cpu.gpr[3];
            const u32 flags = cpu.mem_read32(system + 0x18u);
            if ((flags & 1u) == 0u || (flags & 8u) != 0u || cpu.mem_read32(system + 0x110u) == 0u) return eu::serve_ptank_fill(cpu);
            cpu.mem_write32(system + 0x144u, 1u);  // the tail takes its early exit
            const bool served = eu::serve_ptank_fill(cpu);
            cpu.mem_write32(system + 0x144u, 0u);
            (void)lr; (void)sp0;
            return served;
        }, false, &caught_tail);
        require(caught_tail > 0, "negative control: a changed tail input was not caught");
        run_ptank_case(original, native, 6000, [](Cpu& cpu) {  // the lock count of the unit list off by one unit
            const u32 system = cpu.gpr[3];
            const u32 count = cpu.mem_read32(system + 0x110u);
            const bool served = eu::serve_ptank_fill(cpu);
            cpu.mem_write32(kPtObject + 4u, count + 1u);
            return served;
        }, false, &caught_order);
        require(caught_order > 0, "negative control: a wrong vertex count was not caught");

        if (g_failures != 0) {
            std::fprintf(stderr, "FAIL effect update contract: %d mismatches of %d calls\n", g_failures, g_checked);
            return 1;
        }
        std::printf("PASS effect update contract: %d calls compared (controls caught: NI %u, dt %u, perturbed alpha %u, unfused %u, "
                    "ptank stream %u, wrap %u, unlock %u, tail %u, count %u; %u declines)\n",
                    g_checked, caught_ni, caught_dt, caught_alpha, caught_unfused, caught_stream, caught_wrap, caught_lock, caught_tail,
                    caught_order, ptank_declined);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL effect update contract: %s\n", error.what());
        return 1;
    }
}
