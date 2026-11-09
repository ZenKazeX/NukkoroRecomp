// liveness-roots: reference-interpreter-only
// Deliberate original-game bug fixes (gekko/game_errata.h, docs/GAME_ERRATA.md), proven on the ORIGINAL words of the
// game's DOL in the reference interpreter.
//
// E1 EnemyBaseAI lock byte: the real constructor 0x8019F1D4 runs on an object whose memory holds a chosen garbage
// lock byte (+0x34), up to the return of its StateMachine::Start (0x8019F528, returning to 0x8019F24C). Checked:
//   - without the fix, a nonzero byte makes the AI skip its initial state (state stays 0; the bug reproduces - the
//     oracle), 0 does not;
//   - with the fix (the errata entry run at the constructor's entry, as the installed hook runs it), every byte -
//     0x00, the baseline's skipping 0x21 and 0xB9, the bridge run's 0x80, 0xFF - enters the intended initial state:
//     current state = EnemyAIState_EnemyBaseAI_None (0x805F03F4, vtable 0x8053B98C), its enter (0x801A0000) stored the
//     owner, lock byte 0;
//   - errata::install registers the hook at the constructor, and SHADOW_ERRATA=0 does not.
//
// E2 PlayerMotion toggle counter: the real reader ShadowMotion::update sub0 (0x800B4B3C) runs on a motion whose +0x58
// object answers vf +0x3C with 0 and has bit 0x18 set in its flag set (+0xA8), and whose own vtable's +0x3C / +0x40
// (vf13 / vf14) are stand-ins that record which one ran. Checked:
//   - without the fix the branch follows the garbage's parity (the oracle: the bug), the counter incremented;
//   - with the fix (the errata entry run at the base constructor's entry, as the hook runs it) every garbage value
//     takes the same branch;
//   - the base constructor 0x800AAE18 and ShadowMotion's 0x800B5844 store nothing at +0x3EC (their words scanned);
//   - errata::install registers the hook at the base constructor.
//
// E3 land pattern clocks: the real set init (0x8004F284), update (0x8004F3C4) and frame choice (0x8004F0FC) run an
// old land block's set, an object's set and a new land block's set made 37 frames later, children of task group 3
// whose handles TObjLand's slots hold (the object's only in a free slot). Checked:
//   - without the fix the new set's clock stays behind and it chooses other keys of a period-64 pattern (the bug);
//   - with the fix (the errata entry run where 0x80038148 is entered) both clocks are bit-identical every frame of
//     200 and choose the same keys; the object's set keeps its clock;
//   - 0x80038148's only call site is the RG1 callback 0x80046438; the init and the update store the clock;
//   - errata::install registers the hook at 0x80038148, and SHADOW_ERRATA_E3=0 leaves it out.
//
// usage: game_errata_tests <sys/main.dol>

#include "gekko/cpu.h"
#include "gekko/game_errata.h"
#include "gekko/guest_memory.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;

constexpr u32 kStartReturn = 0x8019F24Cu;     // the constructor, right after StateMachine::Start returned
constexpr u32 kNoneState = 0x805F03F4u;       // the None state's function-local static (guard byte at -4)
constexpr u32 kNoneVtable = 0x8053B98Cu;
constexpr u32 kR13 = 0x805EC500u, kR2 = 0x805FA780u;
constexpr u32 kObject = 0x81700000u, kObjectSize = 0x100u, kStack = 0x817E0000u, kFinish = 0x817FF000u;

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

std::vector<u8> read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}

struct Outcome {
    bool returned = false;
    u32 state = 0, owner_in_state = 0;
    u8 lock = 0;
};

// One construction on a fresh machine: the object's memory is filled with `fill`, its lock byte set to `garbage`.
Outcome construct(const std::vector<u8>& dol, u8 garbage, bool fixed) {
    GuestMemory bus;
    Cpu cpu{bus};
    const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
    for (u32 i = 0; i < 18u; ++i) {
        const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
        if (!size) continue;
        require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
        bus.write_block(address, dol.data() + offset, size);
    }
    require(bus.read32(errata::kEnemyBaseAiConstructor) == 0x9421FF90u, "unexpected EnemyBaseAI constructor words");
    cpu.reset();
    cpu.gpr[2] = kR2;
    cpu.gpr[13] = kR13;
    cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
    for (u32 i = 0; i < kObjectSize; i += 4u) bus.write32(kObject + i, 0xA5A5A5A5u);  // heap garbage
    bus.write8(kObject + errata::kEnemyBaseAiLockOffset, garbage);

    cpu.gpr[3] = kObject;
    cpu.gpr[4] = 1u;  // the derived constructors' "own the +0x74 block" flag
    cpu.gpr[1] = kStack;
    cpu.lr = kFinish;
    cpu.pc = errata::kEnemyBaseAiConstructor;
    errata::Stats stats;
    if (fixed) errata::enemy_base_ai_constructor_entry(cpu, stats);
    u64 steps = 0;
    while (cpu.pc != kStartReturn && !cpu.stopped() && steps++ < 1'000'000ull) cpu.run_steps(1u);
    Outcome out;
    out.returned = cpu.pc == kStartReturn && !cpu.stopped();
    out.state = bus.read32(kObject + 0x2Cu);
    out.owner_in_state = bus.read32(kNoneState + 4u);
    out.lock = bus.read8(kObject + errata::kEnemyBaseAiLockOffset);
    if (fixed) check(stats.ai_constructed == 1u && stats.ai_lock_cleared == (garbage != 0u ? 1u : 0u), "fix stats");
    if (out.returned && out.state == kNoneState) check(bus.read32(kNoneState) == kNoneVtable, "None state vtable");
    return out;
}

// ---- E2 --------------------------------------------------------------------------------------------------------
constexpr u32 kToggleReader = 0x800B4B3Cu;             // ShadowMotion::update sub0
constexpr u32 kMotion = 0x81710000u, kCore = 0x81711000u;  // the motion and its +0x58 object
constexpr u32 kMotionVtable = 0x81712000u, kCoreVtable = 0x81712100u;
constexpr u32 kAnswerZero = 0x81713000u, kVf13 = 0x81713010u, kVf14 = 0x81713020u;
int g_branch = 0;  // 13 or 14: which stand-in ran

void answer_zero(Cpu& c) {
    c.gpr[3] = 0u;
    c.pc = c.lr;
}
void vf13(Cpu& c) {
    g_branch = 13;
    c.pc = c.lr;
}
void vf14(Cpu& c) {
    g_branch = 14;
    c.pc = c.lr;
}

struct Toggle {
    bool returned = false;
    int branch = 0;
    u32 counter = 0;
};

// One update of the toggle on a fresh machine whose motion's +0x3EC holds `garbage`; `fixed` runs E2 first.
Toggle toggle(const std::vector<u8>& dol, u32 garbage, bool fixed) {
    GuestMemory bus;
    Cpu cpu{bus};
    const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
    for (u32 i = 0; i < 18u; ++i) {
        const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
        if (!size) continue;
        require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
        bus.write_block(address, dol.data() + offset, size);
    }
    require(bus.read32(kToggleReader) == 0x9421FFF0u, "unexpected ShadowMotion::update sub0 words");
    RecompiledCode code;
    std::vector<RecompiledCode::Entry> entries = {{kAnswerZero, 4u, answer_zero, "vf +0x3C: 0"},
                                                  {kVf13, 4u, vf13, "ShadowMotion vf13 stand-in"},
                                                  {kVf14, 4u, vf14, "ShadowMotion vf14 stand-in"}};
    code.install(entries.data(), entries.size());
    cpu.attach_recompiled(&code);
    cpu.reset();
    cpu.gpr[2] = kR2;
    cpu.gpr[13] = kR13;
    cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
    for (u32 i = 0; i < 0x868u; i += 4u) bus.write32(kMotion + i, 0xA5A5A5A5u);  // heap garbage
    for (u32 i = 0; i < 0x100u; i += 4u) bus.write32(kCore + i, 0u);
    bus.write32(kMotion + 0x18u, kMotionVtable);
    bus.write32(kMotionVtable + 0x3Cu, kVf13);
    bus.write32(kMotionVtable + 0x40u, kVf14);
    bus.write32(kMotion + 0x58u, kCore);
    bus.write32(kCore + 0x38u, kCoreVtable);   // the +0x58 object's vtable pointer as the reader loads it
    bus.write32(kCoreVtable + 0x3Cu, kAnswerZero);
    bus.write32(kCore + 0xA8u, 1u << 0x18);    // bit 0x18 of the flag set (word 0, bit 24)
    bus.write32(kMotion + errata::kPlayerMotionToggleOffset, garbage);
    errata::Stats stats;
    if (fixed) {
        cpu.gpr[3] = kMotion;
        errata::player_motion_constructor_entry(cpu, stats);
        check(stats.motion_constructed == 1u && stats.motion_counter_cleared == (garbage != 0u ? 1u : 0u), "E2 stats");
    }
    cpu.gpr[3] = kMotion;
    cpu.gpr[1] = kStack;
    cpu.lr = kFinish;
    cpu.pc = kToggleReader;
    g_branch = 0;
    u64 steps = 0;
    while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 100'000ull) cpu.run_steps(1u);
    Toggle out;
    out.returned = cpu.pc == kFinish && !cpu.stopped();
    out.branch = g_branch;
    out.counter = bus.read32(kMotion + errata::kPlayerMotionToggleOffset);
    return out;
}

// Whether the words of [start, start + size) hold a store with displacement +0x3EC (stw / sth / stb / stmw / stfs).
bool stores_counter(const std::vector<u8>& dol, u32 start, u32 size) {
    const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
    for (u32 i = 0; i < 18u; ++i) {
        const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), length = word(0x90u + i * 4u);
        if (!length || start < address || start + size > address + length) continue;
        for (u32 at = start; at < start + size; at += 4u) {
            const u32 w = load_be32(dol.data() + offset + (at - address));
            const u32 op = w >> 26;
            const bool store = op == 36u || op == 37u || op == 38u || op == 39u || op == 44u || op == 45u ||
                               op == 47u || op == 52u || op == 53u || op == 54u || op == 55u;
            if (store && (w & 0xFFFFu) == errata::kPlayerMotionToggleOffset) return true;
        }
        return false;
    }
    throw std::runtime_error("constructor outside the DOL's sections");
}

// ---- E3 --------------------------------------------------------------------------------------------------------
constexpr u32 kPatternInit = 0x8004F284u;    // the set's init (this, list, count): clock +0x34 = 0
constexpr u32 kPatternUpdate = 0x8004F3C4u;  // the set's update (this, f1 dt): clock += 60 dt
constexpr u32 kPatternFrame = 0x8004F0FCu;   // the frame choice (this, entry) -> r3 key index
constexpr u32 kRg1Callback = 0x80046438u, kRg1CallbackSize = 244u;
constexpr u32 kGroup3 = 0x81720000u, kOtherTask = 0x81723000u, kPatternDef = 0x81725000u;
constexpr u32 kSetOld = 0x81721000u, kSetObject = 0x81722000u, kSetNew = 0x81724000u;
constexpr u32 kHandleOld = 0x00070003u, kHandleObject = 0x00090001u, kHandleNew = 0x000B0002u;
constexpr u32 kOldAge = 37u;     // frames the old block's set ran before the new block loaded (not a multiple of 4)
constexpr u32 kObjectAge = 23u;  // ... the object's set
constexpr u32 kRunFrames = 200u;

void load_dol(GuestMemory& bus, const std::vector<u8>& dol) {
    const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
    for (u32 i = 0; i < 18u; ++i) {
        const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
        if (!size) continue;
        require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
        bus.write_block(address, dol.data() + offset, size);
    }
}

// Runs the original function at `pc` to its return.
bool call(Cpu& cpu, u32 pc, u32 r3, u32 r4, u32 r5, float f1) {
    cpu.gpr[3] = r3;
    cpu.gpr[4] = r4;
    cpu.gpr[5] = r5;
    cpu.fpr[1].bits[0] = cpu.fpr[1].bits[1] = double_to_bits(static_cast<double>(f1));
    cpu.gpr[1] = kStack;
    cpu.lr = kFinish;
    cpu.pc = pc;
    u64 steps = 0;
    while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 100'000ull) cpu.run_steps(1u);
    return cpu.pc == kFinish && !cpu.stopped();
}

struct Patterns {
    bool returned = true;
    u32 frames_apart = 0;        // frames of the run where the two land sets chose different keys
    u32 clocks_apart = 0;        // ... where their clocks differ in any bit
    bool object_kept = false;    // the object set's clock was not touched by the fix
    errata::Stats stats;
};

// An old land block's pattern set (running kOldAge frames), an object's set, an unrelated task and a new land block's
// set made last, all children of task group 3 in creation order; TObjLand holds the two land handles (and a free
// slot a stale one). With `fixed`, E3 runs where 0x80038148 is entered: after the new set was made.
Patterns patterns(const std::vector<u8>& dol, bool fixed) {
    GuestMemory bus;
    Cpu cpu{bus};
    load_dol(bus, dol);
    require(bus.read32(kPatternUpdate) == 0x9421FFF0u && bus.read32(kPatternInit) == 0x9421FF90u,
            "unexpected TTextureChange words");
    cpu.reset();
    cpu.gpr[2] = kR2;
    cpu.gpr[13] = kR13;
    cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
    const float dt = static_cast<float>(1.0 / 60.0);
    // Task group 3 and its children (+0x14 first child, +0x0C next, +0x18 vtable, +0x2C handle).
    bus.write32(errata::kTaskGroups + 4u + 4u * errata::kPatternGroup, kGroup3);
    bus.write32(kGroup3 + errata::kTaskFirstChild, kSetOld);
    const u32 order[] = {kSetOld, kSetObject, kOtherTask, kSetNew};
    for (u32 i = 0; i < 4u; ++i) {
        bus.write32(order[i] + errata::kTaskNext, i + 1u < 4u ? order[i + 1u] : 0u);
        bus.write32(order[i] + errata::kTaskVtable, order[i] == kOtherTask ? 0x8051E504u : errata::kTextureChangeVtable);
    }
    bus.write32(kSetOld + errata::kTextureChangeHandle, kHandleOld);
    bus.write32(kSetObject + errata::kTextureChangeHandle, kHandleObject);
    bus.write32(kOtherTask + errata::kTextureChangeHandle, kHandleOld);  // not a set: its word must not matter
    bus.write32(kSetNew + errata::kTextureChangeHandle, kHandleNew);
    // TObjLand: block slot 0 (loaded) layer 13 (_G_) = the old set, layer 0 made no set ({0, -1}); slot 4 (RG1 done)
    // layer 8 (_AF_) = the new set; slot 7 free, with a stale handle of the object's.
    const auto layer = [](u32 slot, u32 k) {
        return errata::kObjLand + slot * errata::kObjLandSlotSize + errata::kObjLandLayerHandle +
               k * errata::kObjLandLayerStride;
    };
    for (u32 i = 0; i < errata::kObjLandSlots * errata::kObjLandSlotSize; i += 4u) bus.write32(errata::kObjLand + i, 0u);
    bus.write32(errata::kObjLand + 0u * errata::kObjLandSlotSize, 5u);
    bus.write32(layer(0u, 13u), kHandleOld);
    bus.write32(layer(0u, 0u), 0x0000FFFFu);
    bus.write32(errata::kObjLand + 4u * errata::kObjLandSlotSize, 4u);
    bus.write32(layer(4u, 8u), kHandleNew);
    bus.write32(layer(7u, 13u), kHandleObject);
    // A pattern: period 64, 16 keys every 4 frames (the corridor light's shape).
    bus.write32(kPatternDef, 64u);
    bus.write32(kPatternDef + 0x48u, 16u);
    for (u32 k = 0; k < 16u; ++k) {
        bus.write32(kPatternDef + 0x4Cu + 8u * k, 4u * k);
        bus.write32(kPatternDef + 0x50u + 8u * k, 1u + (k < 8u ? k : 15u - k));
    }
    Patterns out;
    // The old block loads first and runs; the object appears kObjectAge frames before the new block.
    out.returned &= call(cpu, kPatternInit, kSetOld, 0u, 0u, 0.0f);
    for (u32 f = 0; f < kOldAge; ++f) {
        if (f == kOldAge - kObjectAge) out.returned &= call(cpu, kPatternInit, kSetObject, 0u, 0u, 0.0f);
        out.returned &= call(cpu, kPatternUpdate, kSetOld, 0u, 0u, dt);
        if (f >= kOldAge - kObjectAge) out.returned &= call(cpu, kPatternUpdate, kSetObject, 0u, 0u, dt);
    }
    out.returned &= call(cpu, kPatternInit, kSetNew, 0u, 0u, 0.0f);
    check(bus.read32(kSetNew + errata::kTextureChangeClock) == 0u, "E3: a new set's clock starts at 0 (0x8004F284)");
    const u32 object_clock = bus.read32(kSetObject + errata::kTextureChangeClock);
    if (fixed) errata::land_layer_ready_entry(cpu, out.stats);
    out.object_kept = bus.read32(kSetObject + errata::kTextureChangeClock) == object_clock;
    bus.write32(kSetOld + 0x44u, kPatternDef);  // each set's first entry: the pattern (the init cleared +0x44..)
    bus.write32(kSetNew + 0x44u, kPatternDef);
    for (u32 f = 0; f < kRunFrames; ++f) {
        for (const u32 set : {kSetOld, kSetObject, kSetNew}) out.returned &= call(cpu, kPatternUpdate, set, 0u, 0u, dt);
        out.returned &= call(cpu, kPatternFrame, kSetOld, kSetOld + 0x44u, 0u, 0.0f);
        const u32 old_key = cpu.gpr[3];
        out.returned &= call(cpu, kPatternFrame, kSetNew, kSetNew + 0x44u, 0u, 0.0f);
        out.frames_apart += cpu.gpr[3] != old_key ? 1u : 0u;
        out.clocks_apart += bus.read32(kSetOld + errata::kTextureChangeClock) !=
                                    bus.read32(kSetNew + errata::kTextureChangeClock)
                                ? 1u
                                : 0u;
    }
    return out;
}

// The `bl` sites of `target` in the DOL's text.
std::vector<u32> call_sites(const std::vector<u8>& dol, u32 target) {
    std::vector<u32> sites;
    const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
    for (u32 i = 0; i < 7u; ++i) {  // text sections
        const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), length = word(0x90u + i * 4u);
        for (u32 at = 0; at + 4u <= length; at += 4u) {
            const u32 w = load_be32(dol.data() + offset + at);
            if ((w & 0xFC000003u) != 0x48000001u) continue;
            const u32 displacement = (w & 0x03FFFFFCu) | ((w & 0x02000000u) != 0u ? 0xFC000000u : 0u);
            if (address + at + displacement == target) sites.push_back(address + at);
        }
    }
    return sites;
}

// Whether [start, start + size) holds the word `w`.
bool holds_word(const std::vector<u8>& dol, u32 start, u32 size, u32 w) {
    const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
    for (u32 i = 0; i < 18u; ++i) {
        const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), length = word(0x90u + i * 4u);
        if (!length || start < address || start + size > address + length) continue;
        for (u32 at = start; at < start + size; at += 4u)
            if (load_be32(dol.data() + offset + (at - address)) == w) return true;
        return false;
    }
    throw std::runtime_error("function outside the DOL's sections");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::printf("usage: game_errata_tests <sys/main.dol>\n");
        return 2;
    }
    try {
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() > 0x100u, "cannot read the DOL");
        for (const u8 garbage : {u8{0x00}, u8{0x21}, u8{0xB9}, u8{0x80}, u8{0xFF}}) {
            const Outcome original = construct(dol, garbage, false);
            const Outcome fixed = construct(dol, garbage, true);
            std::printf("lock 0x%02X: original state 0x%08X lock 0x%02X | fixed state 0x%08X owner 0x%08X lock 0x%02X\n",
                        garbage, original.state, original.lock, fixed.state, fixed.owner_in_state, fixed.lock);
            check(original.returned && fixed.returned, "the constructor reached Start's return");
            // The oracle: the original freezes exactly the nonzero bytes.
            check((original.state == kNoneState) == (garbage == 0u), "original: the initial state is skipped iff the lock byte is nonzero");
            // The fix: every AI enters its intended initial state.
            check(fixed.state == kNoneState, "fixed: the AI entered EnemyAIState_EnemyBaseAI_None");
            check(fixed.owner_in_state == kObject, "fixed: the None state's enter recorded its owner");
            check(fixed.lock == 0u, "fixed: the lock byte is 0 after Start");
        }
        // E2: the original's branch follows the garbage's parity (new value even -> vf13, odd -> vf14); fixed, every
        // garbage value starts from 0, so the first toggle always takes vf14. Westopolis's real leftover 0x810132E0
        // and the native read's flipped values from docs/NATIVE_WORKERS.md 5a are among the inputs.
        for (const u32 garbage : {0x00000000u, 0x810132E0u, 0xFDC301FFu, 0x00800000u, 0x57533577u, 0xA5A5A5A5u}) {
            const Toggle original = toggle(dol, garbage, false);
            const Toggle fixed = toggle(dol, garbage, true);
            std::printf("E2 +0x3EC 0x%08X: original vf%d counter 0x%08X | fixed vf%d counter 0x%08X\n", garbage,
                        original.branch, original.counter, fixed.branch, fixed.counter);
            check(original.returned && fixed.returned, "E2: the reader returned");
            check(original.counter == garbage + 1u, "E2 original: the counter was incremented");
            check(original.branch == (((garbage + 1u) & 1u) ? 14 : 13), "E2 original: the branch follows the parity");
            check(fixed.counter == 1u && fixed.branch == 14, "E2 fixed: the first toggle always takes vf14");
        }
        check(!stores_counter(dol, errata::kPlayerMotionConstructor, 324u),
              "E2: PlayerMotion's constructor stores nothing at +0x3EC");
        check(!stores_counter(dol, 0x800B5844u, 200u), "E2: ShadowMotion's constructor stores nothing at +0x3EC");
        // E3: without the fix a land set made kOldAge frames after the first keeps that offset and chooses other keys;
        // with it, the two clocks are bit-identical every frame (the same keys), the object's set is left alone.
        {
            const Patterns original = patterns(dol, false);
            const Patterns fixed = patterns(dol, true);
            std::printf("E3 %u frames: original keys apart %u, clocks apart %u | fixed keys apart %u, clocks apart %u, "
                        "sets %llu aligned %llu, object kept %d\n",
                        kRunFrames, original.frames_apart, original.clocks_apart, fixed.frames_apart,
                        fixed.clocks_apart, static_cast<unsigned long long>(fixed.stats.land_pattern_sets),
                        static_cast<unsigned long long>(fixed.stats.land_clocks_aligned), fixed.object_kept ? 1 : 0);
            check(original.returned && fixed.returned, "E3: the set functions returned");
            check(original.frames_apart > 0u && original.clocks_apart == kRunFrames,
                  "E3 original: the later block's lights run out of step (the bug)");
            check(fixed.frames_apart == 0u && fixed.clocks_apart == 0u, "E3 fixed: both blocks' lights in step");
            check(fixed.stats.land_layers_ready == 1u && fixed.stats.land_pattern_sets == 2u &&
                      fixed.stats.land_clocks_aligned == 1u,
                  "E3 stats: two land sets, the new one aligned");
            check(fixed.object_kept, "E3: an object's pattern set keeps its own clock");
            // Words: 0x80038148 is called only by the RG1 callback; the init stores the clock (stfs f0,0x34(r26)), the
            // update its sum (stfs f0,0x34(r3)).
            const std::vector<u32> sites = call_sites(dol, errata::kLandLayerReady);
            check(sites.size() == 1u && sites[0] >= kRg1Callback && sites[0] < kRg1Callback + kRg1CallbackSize,
                  "E3: 0x80038148's only call site is in the RG1 callback 0x80046438");
            check(holds_word(dol, kPatternInit, 320u, 0xD01A0034u), "E3: the set's init stores its clock");
            check(holds_word(dol, kPatternUpdate, 92u, 0xD0030034u), "E3: the set's update stores its clock");
        }
        {
            GuestMemory bus;
            Cpu cpu{bus};
            errata::Stats stats;
            check(errata::install(cpu, stats), "install");
            bool hooked = false, hooked_e2 = false, hooked_e3 = false;
            for (const auto& entry : cpu.hle_entries()) {
                hooked |= entry.address == errata::kEnemyBaseAiConstructor;
                hooked_e2 |= entry.address == errata::kPlayerMotionConstructor;
                hooked_e3 |= entry.address == errata::kLandLayerReady;
            }
            check(hooked, "the fix is hooked at the EnemyBaseAI constructor");
            check(hooked_e2, "E2 is hooked at the PlayerMotion constructor");
            check(hooked_e3, "E3 is hooked at 0x80038148");
        }
        {
            _putenv_s("SHADOW_ERRATA_E3", "0");
            GuestMemory bus;
            Cpu cpu{bus};
            errata::Stats stats;
            bool hooked_e3 = false;
            check(errata::install(cpu, stats), "install without E3");
            for (const auto& entry : cpu.hle_entries()) hooked_e3 |= entry.address == errata::kLandLayerReady;
            check(!hooked_e3, "SHADOW_ERRATA_E3=0 leaves E3 out");
            _putenv_s("SHADOW_ERRATA_E3", "");
        }
        {
            _putenv_s("SHADOW_ERRATA", "0");
            GuestMemory bus;
            Cpu cpu{bus};
            errata::Stats stats;
            check(!errata::install(cpu, stats) && cpu.hle_entries().empty(), "SHADOW_ERRATA=0 installs nothing");
            _putenv_s("SHADOW_ERRATA", "");
        }
    } catch (const std::exception& error) {
        std::printf("ERROR: %s\n", error.what());
        return 1;
    }
    std::printf("game errata: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
