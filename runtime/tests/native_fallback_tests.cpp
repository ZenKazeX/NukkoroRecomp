// Execute real ppcrecomp output, including its cold interior-entry path.
#include "gekko/cpu.h"
#include "gekko/cpu_diag.h"
#include "gekko/os_exception_vector.h"
#include "gekko/recomp.h"
#include "fallback_fixture.h"

#include "gekko/ppc_ops.h"
#include "gekko/ppc_instruction_traits.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace gekko;

namespace {
void require(bool ok, const char* detail) {
    if (!ok) throw std::runtime_error(detail);
}

// The OS's exception entry as the guest installs it (__OSExceptionInit, OSSetCurrentContext): the
// vector page with `li r3, <index>` at +0x68,
// the handler in __OSExceptionTable, the current OSContext at low memory 0xD4. The handlers have no
// translated code; each starts with a `li r31,<vector>` marker the reference dispatcher interprets.
constexpr u32 kContext = 0x8000A000u;
constexpr u32 kExternalHandler = 0x80009500u, kProgramHandler = 0x80009700u, kFpHandler = 0x80009800u,
              kDecrementerHandler = 0x80009900u;

struct Fixture {
    GuestMemory bus;
    Cpu cpu{bus};
    Fixture() {
        cpu.reset();
        for (const auto& [vector, index, handler] :
             {std::tuple{Exception::ExternalInterrupt, 4u, kExternalHandler},
              std::tuple{Exception::Program, 6u, kProgramHandler},
              std::tuple{Exception::FpUnavailable, 7u, kFpHandler},
              std::tuple{Exception::Decrementer, 8u, kDecrementerHandler}}) {
            write_os_exception_vector(bus, 0x80000000u | static_cast<u32>(vector), index);
            bus.write32(0x80003000u + 4u * index, handler);
            bus.write32(handler, 0x3BE00000u | static_cast<u32>(vector));  // li r31,<vector>
        }
        bus.write32(0x800000D4u, kContext);
    }
};

// The interrupted SRR0 / SRR1 the native OS entry saved in the OSContext.
u32 saved_srr0(const Fixture& f) { return f.bus.peek32(kContext + 0x198u); }
u32 saved_srr1(const Fixture& f) { return f.bus.peek32(kContext + 0x19Cu); }

// Game time as one value: the time-base and decrementer views and the game clock and schedule behind
// them. Guest code runs in zero game time in either engine (only a wait point moves the clock), so a run
// leaves it exactly as it found it and both engines always agree on it.
struct GameTime {
    u64 time_base, now;
    u32 decrementer;
    std::array<u64, static_cast<std::size_t>(GameEvent::Count)> deadlines;
    explicit GameTime(const Cpu& c)
        : time_base(c.time_base()), now(c.game_clock().now()), decrementer(c.decrementer()),
          deadlines(c.game_clock().deadlines()) {}
    bool operator==(const GameTime& other) const {
        return time_base == other.time_base && now == other.now && decrementer == other.decrementer &&
               deadlines == other.deadlines;
    }
};

void compare(const Cpu& a, const Cpu& b) {
    require(std::memcmp(a.gpr, b.gpr, sizeof(a.gpr)) == 0, "GPR mismatch");
    require(std::memcmp(a.fpr, b.fpr, sizeof(a.fpr)) == 0, "FPR mismatch");
    require(a.spr == b.spr && a.decrementer() == b.decrementer() && a.sr == b.sr,
            "SPR/segment mismatch");
    require(a.pc == b.pc && a.msr() == b.msr() && a.cr == b.cr && a.xer == b.xer &&
                a.lr == b.lr && a.ctr == b.ctr && a.fpscr == b.fpscr, "control state mismatch");
    // There is no retirement count to compare any more: every caller steps the oracle to the end the
    // native run reached and pins the instructions it took to get there (see step_until).
    require(GameTime(a) == GameTime(b), "game time mismatch (time base, decrementer or clock)");
}

// Native code has no instruction count, so the oracle is not stepped "to the native count" any more:
// it is stepped until it reaches the end the native run reached (`pc`; or until it stops, or has taken
// `limit` steps), and the steps it took - the instructions of the path - are returned for the caller to
// pin. (Cpu::interpreted_instructions counts the instructions the interpreter COMPLETED; a faulting one
// does not count.)
u64 step_until(Fixture& oracle, u32 pc, u64 limit) {
    u64 steps = 0;
    while (steps < limit && !oracle.cpu.stopped() && oracle.cpu.pc != pc) {
        oracle.cpu.step();
        ++steps;
    }
    return steps;
}

void traps(RecompiledCode& code) {
    auto native = std::make_unique<Fixture>();
    auto oracle = std::make_unique<Fixture>();
    constexpr u32 values[] = {0u, 1u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu};
    unsigned cases = 0;
    for (unsigned index = 0; index < sizeof(kTrapWords) / sizeof(kTrapWords[0]); ++index) {
        const u32 start = 0x80004000u + index * 0x20u;
        for (u32 a : values) for (u32 b : values) for (bool interior : {false, true}) {
            for (Fixture* fixture : {native.get(), oracle.get()}) {
                fixture->cpu.reset();
                fixture->cpu.set_entry(start + (interior ? 4u : 0u));
                fixture->cpu.gpr[3] = a;
                fixture->cpu.gpr[4] = b;
                fixture->cpu.lr = 0x80009000u;
                fixture->cpu.set_msr(fixture->cpu.msr() | (kMsrEE | ((cases & 1u) ? kMsrIP : 0u)));
                fixture->bus.write32(start, 0x38A50001u);
                fixture->bus.write32((start + 4u), kTrapWords[index]);
                fixture->bus.write32((start + 8u), 0x38C60001u);
                fixture->bus.write32((start + 12u), 0x4E800020u);
            }
            native->cpu.attach_recompiled(&code);
            oracle->cpu.attach_recompiled(nullptr);
            const GameTime time(native->cpu);
            const u64 before = g_non_native_ppc_executions.load();
            require(native->cpu.run_steps(1u) == 1u, "a generated trap function must be one dispatch");
            const u64 interpreted = g_non_native_ppc_executions.load() - before;
            // The native run either trapped - through the OS entry to the program handler, SRR0 at the
            // trap saved in the OSContext (MSR[IP], set in odd cases, selects nothing: the OS entry is
            // the only exception entry) - or returned to LR. The interpreter must reach the same place
            // over the same path: the addi before the trap (entry only), the trap itself, and the addi
            // and blr after it when the trap is not taken.
            const bool trapped = native->cpu.pc == kProgramHandler;
            require(trapped || native->cpu.pc == 0x80009000u, "translated trap ended neither at its handler nor at LR");
            require(!trapped || saved_srr0(*native) == start + 4u, "the trap's SRR0 must address the trap");
            require(step_until(*oracle, native->cpu.pc, 4u) == (interior ? 0u : 1u) + (trapped ? 1u : 3u),
                    "the interpreter reached the native end over a different path");
            compare(native->cpu, oracle->cpu);
            require(GameTime(native->cpu) == time, "a translated trap moved game time");
            require(interpreted == 0u && native->cpu.interpreted_instructions() == 0u,
                    "translated trap used the interpreter");
            ++cases;
        }
    }
    std::cout << "PASS " << cases << " generated trap cases (all TO masks, signed/unsigned edges, interior entries)\n";
}

void hooks(RecompiledCode& code) {
    for (bool interior : {false, true}) for (bool served : {false, true})
        for (bool verify : {false, true}) {
            auto fixture = std::make_unique<Fixture>();
            Cpu& cpu = fixture->cpu;
            const u32 entry = 0x80008000u + (interior ? 4u : 0u);
            fixture->bus.write32(0x80008000u, 0x38630001u);
            fixture->bus.write32(0x80008004u, 0x38840001u);
            fixture->bus.write32(0x80008008u, 0x4E800020u);
            cpu.set_entry(entry);
            cpu.lr = 0x80009000u;
            cpu.attach_recompiled(&code);
            unsigned hits = 0;
            cpu.install_hle(entry, [&](Cpu& c) {
                ++hits;
                ++c.gpr[7];
                return served;
            }, "fallback test hook");
            cpu.set_verify_recompiled(verify);
            const u64 before = g_non_native_ppc_executions.load();
            const GameTime time(cpu);
            require(cpu.run_steps(1u) == 1u, "a hooked translated address must be one dispatch");
            require(cpu.pc == cpu.lr, "hook route did not return to LR");
            require(cpu.gpr[7] == 1u, "hook side effects executed twice");
            require(cpu.gpr[3] == ((!served && !interior) ? 1u : 0u) &&
                        cpu.gpr[4] == (served ? 0u : 1u), "wrong native suffix after hook");
            // A served call is the handler's alone: its served-call ledger records it and no translated
            // instruction runs (r3/r4 above). A declined one runs the translated suffix from the hooked
            // address - three instructions from the entry, two from the interior.
            require(cpu.hle_entries().size() == 1u && cpu.hle_entries().front().hits == (served ? 1u : 0u),
                    "hook classification mismatch: the served-call ledger must hold exactly the served call");
            require(GameTime(cpu) == time, "the hook route moved game time");
            // Exactly once in every mode. The verified arm used to expect 2: the
            // oracle's native pass ran the handler and its interpreter replay
            // ran it again (audit finding oracle-reference-4).
            require(hits == 1u, "hook invocation count mismatch");
            if (verify) {
                const auto& report = cpu.verify_report();
                // A served call is host code with nothing to compare; a declined
                // call's translated suffix is verified as its own segment.
                require(report.mismatches == 0u &&
                            (served ? (report.skipped_hle == 1u && report.verified == 0u)
                                    : (report.verified == 1u && report.skipped_hle == 0u)),
                        "hook route bypassed or failed native verification");
                require(report.verified_instructions == (served ? 0u : (interior ? 2u : 3u)),
                        "the oracle must replay exactly the declined call's translated suffix");
            } else {
                require(g_non_native_ppc_executions.load() == before && cpu.interpreted_instructions() == 0u,
                        "hook route used the interpreter");
            }
        }
    std::cout << "PASS native hook dispatch (served/declined, entry/interior, plain/verified)\n";
}

// ---- instruction semantics through the real generator (audit batch b2) ----
//
// Each kSem* program is translated by ppcrecomp. It runs natively (no
// interpreter instruction may execute) and through the reference interpreter
// from identical state; both results must agree with each other AND with a
// value computed here from the manual's RTL. Engine-vs-engine agreement alone
// is what let all of these defects survive.

void require_that(bool ok, const std::string& detail) {
    if (!ok) throw std::runtime_error(detail);
}

struct SemProgram {
    const char* name;
    u32 address;
    const unsigned* words;
    std::size_t count;
};
#define GEKKO_SEM_PROGRAM(Name)                                    \
    SemProgram{#Name, kSem##Name##Address, kSem##Name##Words,       \
               sizeof(kSem##Name##Words) / sizeof(kSem##Name##Words[0])}

constexpr u32 kSemReturn = 0x80009000u;
constexpr u32 kSemData = 0x80300000u;  // in the program image

void execute_both(RecompiledCode& code, const SemProgram& program, Fixture& native, Fixture& oracle,
                  const std::function<void(Fixture&)>& setup) {
    const std::string name = program.name;
    require_that(code.lookup(program.address) != nullptr,
                 name + ": the generator did not translate the program");
    for (Fixture* fixture : {&native, &oracle}) {
        for (std::size_t index = 0; index < program.count; ++index) {
            fixture->bus.write32((program.address) + 4u * static_cast<u32>(index),
                                 program.words[index]);
        }
        setup(*fixture);
        fixture->cpu.set_entry(program.address);
        fixture->cpu.lr = kSemReturn;
    }
    native.cpu.attach_recompiled(&code);
    oracle.cpu.attach_recompiled(nullptr);
    // The comparison is differential: both engines must start from the same architected state and game
    // time (what the retirement counts in step used to stand for).
    try {
        compare(native.cpu, oracle.cpu);
    } catch (const std::exception& error) {
        throw std::runtime_error(name + ": fixtures out of step: " + error.what());
    }
    const GameTime time(native.cpu);
    const u64 interpreted = g_non_native_ppc_executions.load();
    require_that(native.cpu.run_steps(1u) == 1u, name + ": the native run must be one dispatch");
    require_that(g_non_native_ppc_executions.load() == interpreted && native.cpu.interpreted_instructions() == 0u,
                 name + ": native run used the interpreter");
    require_that(!native.cpu.stopped() && native.cpu.pc == kSemReturn,
                 name + ": native run did not return");
    // The native return is pinned to the program: the interpreter reaches the same return over exactly
    // the program's instructions, every one completed, and the two results must then be identical.
    const u64 completed = oracle.cpu.interpreted_instructions();
    const u64 steps = step_until(oracle, kSemReturn, program.count + 1u);
    require_that(!oracle.cpu.stopped(), name + ": interpreter stopped");
    require_that(oracle.cpu.pc == kSemReturn && steps == program.count &&
                     oracle.cpu.interpreted_instructions() - completed == program.count,
                 name + ": the interpreter did not return after exactly the program's instructions");
    compare(native.cpu, oracle.cpu);
    require_that(GameTime(native.cpu) == time, name + ": guest code moved game time");
    require_that(native.cpu.reservation == oracle.cpu.reservation &&
                     native.cpu.reservation_address == oracle.cpu.reservation_address,
                 name + ": reservation mismatch between engines");
    require_that(native.bus.window_bytes(kSemData, kSemData + 0x400u) ==
                     oracle.bus.window_bytes(kSemData, kSemData + 0x400u),
                 name + ": data memory mismatch between engines");
}

u32 read_data32(Fixture& fixture, u32 effective) {
    return fixture.bus.read32(effective);
}

void update_forms(RecompiledCode& code) {
    auto native = std::make_unique<Fixture>();
    auto oracle = std::make_unique<Fixture>();

    // stwux r1,r1,r11 in the TRK__write_aram prologue: EA = r1 + r11 and the
    // ORIGINAL r1 (the back chain) is stored there.
    execute_both(code, GEKKO_SEM_PROGRAM(TrkPrologue), *native, *oracle, [](Fixture& f) {
        f.cpu.gpr[1] = 0x8055FF38u;
    });
    for (Fixture* f : {native.get(), oracle.get()}) {
        require_that(f->cpu.gpr[11] == 0xFFFFFF68u && f->cpu.gpr[12] == 0x8055FF38u,
                     "TrkPrologue: clrlwi/mr/subfic results");
        require_that(f->cpu.gpr[1] == 0x8055FEA0u, "TrkPrologue: stwux must update r1 to EA");
        require_that(read_data32(*f, 0x8055FEA0u) == 0x8055FF38u,
                     "TrkPrologue: stwux r1,r1,r11 must store the original r1 as the back chain");
    }

    execute_both(code, GEKKO_SEM_PROGRAM(SthuxAliased), *native, *oracle, [](Fixture& f) {
        f.cpu.gpr[5] = 0x80300010u;
        f.cpu.gpr[6] = 0x10u;
    });
    for (Fixture* f : {native.get(), oracle.get()}) {
        require_that(f->cpu.gpr[5] == 0x80300020u && f->bus.read16(kSemData + 0x20u) == 0x0010u,
                     "SthuxAliased: sthux r5,r5,r6 must store the original r5");
    }

    execute_both(code, GEKKO_SEM_PROGRAM(StbuxAliased), *native, *oracle, [](Fixture& f) {
        f.cpu.gpr[5] = 0x80300011u;
        f.cpu.gpr[6] = 0x20u;
    });
    for (Fixture* f : {native.get(), oracle.get()}) {
        require_that(f->cpu.gpr[5] == 0x80300031u && f->bus.read8(kSemData + 0x31u) == 0x11u,
                     "StbuxAliased: stbux r5,r5,r6 must store the original r5");
    }

    execute_both(code, GEKKO_SEM_PROGRAM(Stbux), *native, *oracle, [](Fixture& f) {
        f.cpu.gpr[5] = 0x80300040u;
        f.cpu.gpr[6] = 4u;
        f.cpu.gpr[7] = 0xAABBCCDDu;
        f.bus.write32(kSemData + 0x44u, 0x01020304u);
    });
    for (Fixture* f : {native.get(), oracle.get()}) {
        require_that(f->cpu.gpr[5] == 0x80300044u && f->cpu.gpr[6] == 4u &&
                         f->cpu.gpr[7] == 0xAABBCCDDu &&
                         f->bus.read32(kSemData + 0x44u) == 0xDD020304u,
                     "Stbux: stbux r7,r5,r6 stores one byte at r5+r6 and updates r5");
    }

    // stfsux, stfdux, lwzux, lbzux, lhzux, lhaux, lfsux, lfdux, each through r5
    // with r6 = 8, so each EA is the previous one plus 8.
    const auto chain_setup = [](Fixture& f) {
        f.cpu.gpr[5] = 0x80300100u;
        f.cpu.gpr[6] = 8u;
        f.cpu.fpr[1].bits[0] = double_to_bits(1.5);
        f.cpu.fpr[2].bits[0] = 0x0123456789ABCDEFull;
        f.bus.write32(kSemData + 0x118u, 0x11223344u);
        f.bus.write8(kSemData + 0x120u, 0x9Au);
        f.bus.write16(kSemData + 0x128u, 0xFEDCu);
        f.bus.write16(kSemData + 0x130u, 0x8001u);
        f.bus.write32(kSemData + 0x138u, 0x40490FDBu);
        f.bus.write64(kSemData + 0x140u, 0x400921FB54442D18ull);
    };
    execute_both(code, GEKKO_SEM_PROGRAM(UpdateChain), *native, *oracle, chain_setup);
    for (Fixture* f : {native.get(), oracle.get()}) {
        const Cpu& c = f->cpu;
        require_that(f->bus.read32(kSemData + 0x108u) == 0x3FC00000u &&
                         f->bus.read64(kSemData + 0x110u) == 0x0123456789ABCDEFull,
                     "UpdateChain: stfsux/stfdux stores");
        require_that(c.gpr[7] == 0x11223344u && c.gpr[8] == 0x9Au && c.gpr[9] == 0xFEDCu &&
                         c.gpr[10] == 0xFFFF8001u,
                     "UpdateChain: lwzux/lbzux/lhzux/lhaux loads");
        const u64 pi_single = double_to_bits(static_cast<double>(bits_to_float(0x40490FDBu)));
        require_that(c.fpr[3].bits[0] == pi_single && c.fpr[3].bits[1] == pi_single &&
                         c.fpr[4].bits[0] == 0x400921FB54442D18ull,
                     "UpdateChain: lfsux/lfdux loads");
        require_that(c.gpr[5] == 0x80300140u && c.gpr[6] == 8u, "UpdateChain: final rA");
    }
    std::cout << "PASS generated update forms (stwux/sthux/stbux rS==rA, stbux, eight chained X-form updates)\n";
}

void reservation(RecompiledCode& code) {
    auto native = std::make_unique<Fixture>();
    auto oracle = std::make_unique<Fixture>();
    for (bool so : {false, true}) {
        // lwarx r6,0,r3; stwcx. r4,0,r3; mfcr r7; stwcx. r5,0,r3; mfcr r8; blr
        execute_both(code, GEKKO_SEM_PROGRAM(Reservation), *native, *oracle, [so](Fixture& f) {
            f.cpu.gpr[3] = 0x80300204u;
            f.cpu.gpr[4] = 0x11111111u;
            f.cpu.gpr[5] = 0x22222222u;
            f.cpu.xer = so ? kXerSO : 0u;
            f.cpu.cr = 0xF0000000u;
            f.cpu.reservation = false;
            f.bus.write32(kSemData + 0x204u, 0xCAFEF00Du);
        });
        const u32 so_bit = so ? 1u : 0u;
        for (Fixture* f : {native.get(), oracle.get()}) {
            const Cpu& c = f->cpu;
            require_that(c.gpr[6] == 0xCAFEF00Du, "Reservation: lwarx loads the word");
            require_that((c.gpr[7] >> 28) == (2u | so_bit),
                         "Reservation: the first stwcx. stores and sets CR0 = EQ|SO");
            require_that((c.gpr[8] >> 28) == so_bit,
                         "Reservation: the second stwcx. has no reservation: CR0 = SO only");
            require_that(f->bus.read32(kSemData + 0x204u) == 0x11111111u,
                         "Reservation: the second stwcx. must not store");
            require_that(!c.reservation && c.reservation_address == 0x80300200u,
                         "Reservation: stwcx. cancels; lwarx recorded the aligned 32-byte block");
        }
    }
    for (bool reserved : {false, true}) {
        execute_both(code, GEKKO_SEM_PROGRAM(StwcxUnreserved), *native, *oracle,
                     [reserved](Fixture& f) {
                         f.cpu.gpr[3] = 0x80300300u;
                         f.cpu.gpr[4] = 0x33333333u;
                         f.cpu.xer = 0u;
                         f.cpu.reservation = reserved;
                         f.cpu.reservation_address = 0x80000000u;  // stwcx. ignores the address
                         f.bus.write32(kSemData + 0x300u, 0x0u);
                     });
        for (Fixture* f : {native.get(), oracle.get()}) {
            require_that(f->bus.read32(kSemData + 0x300u) == (reserved ? 0x33333333u : 0u) &&
                             (f->cpu.gpr[7] >> 28) == (reserved ? 2u : 0u) && !f->cpu.reservation,
                         reserved ? "StwcxUnreserved: a reservation at any address permits the store"
                                  : "StwcxUnreserved: no reservation, no store, EQ clear");
        }
    }
    std::cout << "PASS generated lwarx/stwcx. reservation (store, cancel, failed retry, SO copy)\n";
}

u32 reference_cntlzw(u32 value) {
    u32 count = 0;
    while (count < 32u && (value & 0x80000000u) == 0u) {
        ++count;
        value <<= 1;
    }
    return count;
}

void count_leading_zeros(RecompiledCode& code) {
    auto native = std::make_unique<Fixture>();
    auto oracle = std::make_unique<Fixture>();
    std::vector<u32> patterns = {0u, 0xFFFFFFFFu};
    for (u32 bit = 0; bit < 32u; ++bit) {
        const u32 single = 1u << bit;
        patterns.push_back(single);                      // every single bit
        patterns.push_back(single - 1u);                 // every low mask
        patterns.push_back(0xFFFFFFFFu << bit);          // every prefix mask
        patterns.push_back(0xFFFFFFFFu >> bit);          // every suffix mask
        patterns.push_back(single | ((single - 1u) & 0x5A5A5A5Au));
        patterns.push_back(single | ((single - 1u) & 0xA5A5A5A5u));
    }
    for (u32 value : patterns) {
        const u32 expected = reference_cntlzw(value);
        require_that(ppc::cntlzw(value) == expected,
                     "cntlzw helper (this TU's /arch) disagrees with the bit loop");
        execute_both(code, GEKKO_SEM_PROGRAM(Cntlzw), *native, *oracle,
                     [value](Fixture& f) { f.cpu.gpr[4] = value; });
        require_that(native->cpu.gpr[3] == expected && oracle->cpu.gpr[3] == expected,
                     "generated cntlzw r3,r4 result");
    }
    // Every helper result for a sweep of 2^24 structured values.
    for (u32 index = 0; index < (1u << 24); ++index) {
        const u32 value = index * 0x9E3779B1u;
        const u32 shifted = value >> (index & 31u);
        if (ppc::cntlzw(shifted) != reference_cntlzw(shifted)) require(false, "cntlzw sweep");
    }
    std::cout << "PASS cntlzw (" << patterns.size() << " boundary patterns generated, 2^24 helper sweep)\n";
}

struct SumReference {
    u32 value;
    bool carry;
    bool overflow;
};

// x + y + carry_in in exact 64-bit unsigned (CA) and signed (OV) arithmetic.
SumReference exact_sum(u32 x, u32 y, u32 carry_in) {
    const u64 wide = static_cast<u64>(x) + y + carry_in;
    const s64 signed_wide = static_cast<s64>(static_cast<s32>(x)) + static_cast<s32>(y) + carry_in;
    return {static_cast<u32>(wide), (wide >> 32) != 0u,
            signed_wide < -2147483648ll || signed_wide > 2147483647ll};
}

void overflow_forms(RecompiledCode& code) {
    auto native = std::make_unique<Fixture>();
    auto oracle = std::make_unique<Fixture>();
    constexpr u32 edges[] = {0u, 1u, 0x40000000u, 0x7FFFFFFEu, 0x7FFFFFFFu,
                             0x80000000u, 0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu};
    enum class X { A, NotA };
    enum class Y { B, Zero, MinusOne };
    enum class Cin { Zero, One, Ca };
    struct Form {
        SemProgram program;
        X x;
        Y y;
        Cin cin;
        bool sets_carry;
    };
    const Form forms[] = {
        {GEKKO_SEM_PROGRAM(Addo), X::A, Y::B, Cin::Zero, false},
        {GEKKO_SEM_PROGRAM(Addco), X::A, Y::B, Cin::Zero, true},
        {GEKKO_SEM_PROGRAM(Addeo), X::A, Y::B, Cin::Ca, true},
        {GEKKO_SEM_PROGRAM(Addzeo), X::A, Y::Zero, Cin::Ca, true},
        {GEKKO_SEM_PROGRAM(Addmeo), X::A, Y::MinusOne, Cin::Ca, true},
        {GEKKO_SEM_PROGRAM(Subfo), X::NotA, Y::B, Cin::One, false},
        {GEKKO_SEM_PROGRAM(Subfco), X::NotA, Y::B, Cin::One, true},
        {GEKKO_SEM_PROGRAM(Subfeo), X::NotA, Y::B, Cin::Ca, true},
        {GEKKO_SEM_PROGRAM(Subfzeo), X::NotA, Y::Zero, Cin::Ca, true},
        {GEKKO_SEM_PROGRAM(Subfmeo), X::NotA, Y::MinusOne, Cin::Ca, true},
        {GEKKO_SEM_PROGRAM(Nego), X::NotA, Y::Zero, Cin::One, false},
    };
    unsigned cases = 0;
    for (const Form& form : forms) {
        for (u32 a : edges) for (u32 b : edges) for (bool ca : {false, true})
            for (u32 flags : {0u, kXerSO, kXerOV}) {
                if (form.y != Y::B && b != 0u) continue;
                execute_both(code, form.program, *native, *oracle, [&](Fixture& f) {
                    f.cpu.gpr[7] = a;
                    f.cpu.gpr[9] = b;
                    f.cpu.xer = flags | (ca ? kXerCA : 0u);
                });
                const u32 x = form.x == X::A ? a : ~a;
                const u32 y = form.y == Y::B ? b : (form.y == Y::Zero ? 0u : 0xFFFFFFFFu);
                const u32 cin = form.cin == Cin::Zero ? 0u : (form.cin == Cin::One ? 1u : (ca ? 1u : 0u));
                const SumReference expected = exact_sum(x, y, cin);
                const bool carry = form.sets_carry ? expected.carry : ca;
                const bool so = (flags & kXerSO) != 0u || expected.overflow;
                const u32 xer = (so ? kXerSO : 0u) | (expected.overflow ? kXerOV : 0u) |
                                (carry ? kXerCA : 0u);
                const s32 signed_value = static_cast<s32>(expected.value);
                const u32 cr0 = (signed_value < 0 ? 8u : (signed_value > 0 ? 4u : 2u)) | (so ? 1u : 0u);
                for (Fixture* f : {native.get(), oracle.get()}) {
                    if (f->cpu.gpr[5] != expected.value || f->cpu.xer != xer ||
                        (f->cpu.cr >> 28) != cr0) {
                        char row[256];
                        std::snprintf(row, sizeof(row),
                                      "%s (%s) rA=0x%08X rB=0x%08X CA=%d xer_in=0x%08X: "
                                      "r5=0x%08X xer=0x%08X cr0=%X, expected r5=0x%08X xer=0x%08X cr0=%X",
                                      form.program.name, f == native.get() ? "native" : "interpreter",
                                      a, b, ca ? 1 : 0, flags, f->cpu.gpr[5], f->cpu.xer,
                                      f->cpu.cr >> 28, expected.value, xer, cr0);
                        require_that(false, row);
                    }
                }
                ++cases;
            }
    }
    std::cout << "PASS " << cases
              << " generated OE forms against exact 64-bit arithmetic (add/addc/adde/addze/addme/"
                 "subf/subfc/subfe/subfze/subfme/neg)\n";
}

// ---- floating-point compares, paired moves, quantised stores (audit batch b6) ----
//
// fpu-5 / paired-single-9: fcmpu/fcmpo and ps_cmpu*/ps_cmpo* write FPCC (never
// FPSCR[C]), VXSNAN for an SNaN, VXVC for the ordered forms (SNaN only when
// VE = 0), FX only on a 0 -> 1 transition (PAGE 397/398, 503-506).
// paired-single-6: ps_sel and the copied halves of ps_sum0/ps_sum1 are bit
// moves (PAGE 528/530/531). paired-single-4: psq_st of NaN/+Inf stores the
// positive overflow value, -Inf the negative one (PAGE 111).
// paired-single-2: the type-0 and integer psq paths against PAGE 110/493/497.

constexpr u64 kFpQNaN = 0x7FF8000000000000ull;
constexpr u64 kFpNegQNaN = 0xFFF8000000000000ull;
constexpr u64 kFpSNaN = 0x7FF4000000000000ull;

void floating_point_forms(RecompiledCode& code) {
    auto native = std::make_unique<Fixture>();
    auto oracle = std::make_unique<Fixture>();
    const u64 one = double_to_bits(1.0), two = double_to_bits(2.0), half = double_to_bits(0.5);

    struct Compare {
        SemProgram program;
        u64 a[2], b[2];
        u32 fpscr_in, code, fpscr_out;
    };
    const Compare compares[] = {
        {GEKKO_SEM_PROGRAM(Fcmpu), {kFpQNaN, 0}, {one, 0}, 0x00010000u, 1u, 0x00011000u},
        {GEKKO_SEM_PROGRAM(Fcmpo), {kFpQNaN, 0}, {one, 0}, 0x00010000u, 1u, 0xA0091000u},
        {GEKKO_SEM_PROGRAM(Fcmpu), {one, 0}, {kFpSNaN, 0}, 0u, 1u, 0xA1001000u},
        {GEKKO_SEM_PROGRAM(Fcmpo), {kFpSNaN, 0}, {one, 0}, 0u, 1u, 0xA1081000u},
        {GEKKO_SEM_PROGRAM(Fcmpo), {kFpSNaN, 0}, {one, 0}, 0x00000080u, 1u, 0xE1001080u},
        {GEKKO_SEM_PROGRAM(Fcmpo), {kFpNegQNaN, 0}, {kFpQNaN, 0}, 0x20080000u, 1u, 0x20081000u},
        {GEKKO_SEM_PROGRAM(Fcmpu), {one, 0}, {two, 0}, 0x0001F000u, 8u, 0x00018000u},
        {GEKKO_SEM_PROGRAM(PsCmpo0), {one, kFpQNaN}, {two, one}, 0u, 8u, 0x00008000u},
        {GEKKO_SEM_PROGRAM(PsCmpu0), {kFpQNaN, one}, {one, one}, 0u, 1u, 0x00001000u},
        {GEKKO_SEM_PROGRAM(PsCmpo0), {kFpQNaN, one}, {one, one}, 0u, 1u, 0xA0081000u},
        {GEKKO_SEM_PROGRAM(PsCmpu1), {kFpQNaN, two}, {kFpQNaN, one}, 0u, 4u, 0x00004000u},
        {GEKKO_SEM_PROGRAM(PsCmpu1), {one, one}, {one, kFpSNaN}, 0u, 1u, 0xA1001000u},
        {GEKKO_SEM_PROGRAM(PsCmpo1), {one, kFpQNaN}, {two, one}, 0u, 1u, 0xA0081000u},
        {GEKKO_SEM_PROGRAM(PsCmpo1), {one, one}, {one, one}, 0x50000040u, 2u, 0x50002040u},
    };
    for (const Compare& test : compares) {
        execute_both(code, test.program, *native, *oracle, [&](Fixture& f) {
            f.cpu.fpr[1].bits[0] = test.a[0];
            f.cpu.fpr[1].bits[1] = test.a[1];
            f.cpu.fpr[2].bits[0] = test.b[0];
            f.cpu.fpr[2].bits[1] = test.b[1];
            f.cpu.fpscr = test.fpscr_in;
            f.cpu.cr = 0xFFFFFFFFu;
        });
        const u32 cr = 0xFFF0FFFFu | (test.code << 16);  // cr3
        for (Fixture* f : {native.get(), oracle.get()}) {
            if (f->cpu.fpscr != test.fpscr_out || f->cpu.cr != cr) {
                char row[200];
                std::snprintf(row, sizeof(row), "%s (%s) fpscr_in 0x%08X: fpscr 0x%08X cr 0x%08X, expected 0x%08X 0x%08X",
                              test.program.name, f == native.get() ? "native" : "interpreter", test.fpscr_in,
                              f->cpu.fpscr, f->cpu.cr, test.fpscr_out, cr);
                require_that(false, row);
            }
        }
    }

    const u64 point_one = 0x3FB999999999999Aull;  // 0.1: not a single
    const u64 single_sum = double_to_bits(static_cast<double>(static_cast<float>(1.0 + 0.1)));
    struct Move {
        SemProgram program;
        u32 d;
        u64 f1[2], f2[2], f5[2];  // frA = f1, frB = f2, frC = f5 (frD may alias one)
        u64 expect[2];
    };
    const Move moves[] = {
        {GEKKO_SEM_PROGRAM(PsSum0), 3, {one, 0}, {0, half}, {two, kFpSNaN}, {double_to_bits(1.5), kFpSNaN}},
        {GEKKO_SEM_PROGRAM(PsSum0), 3, {one, 0}, {0, point_one}, {0, point_one}, {single_sum, point_one}},
        {GEKKO_SEM_PROGRAM(PsSum0Alias), 2, {one, 0}, {two, half}, {0, point_one}, {double_to_bits(1.5), point_one}},
        {GEKKO_SEM_PROGRAM(PsSum1), 3, {one, 0}, {0, half}, {kFpSNaN, two}, {kFpSNaN, double_to_bits(1.5)}},
        {GEKKO_SEM_PROGRAM(PsSum1Alias), 5, {one, 0}, {0, point_one}, {point_one, two}, {point_one, single_sum}},
        {GEKKO_SEM_PROGRAM(PsSel), 3, {one, double_to_bits(-1.0)}, {two, point_one}, {kFpSNaN, half},
         {kFpSNaN, point_one}},
        {GEKKO_SEM_PROGRAM(PsSel), 3, {0x8000000000000000ull, kFpQNaN}, {two, point_one}, {point_one, half},
         {point_one, point_one}},
        {GEKKO_SEM_PROGRAM(PsSelAlias), 1, {double_to_bits(-1.0), one}, {kFpSNaN, two}, {half, point_one},
         {kFpSNaN, point_one}},
    };
    for (const Move& test : moves) {
        execute_both(code, test.program, *native, *oracle, [&](Fixture& f) {
            for (u32 half_index = 0; half_index < 2u; ++half_index) {
                f.cpu.fpr[3].bits[half_index] = 0u;
                f.cpu.fpr[1].bits[half_index] = test.f1[half_index];
                f.cpu.fpr[2].bits[half_index] = test.f2[half_index];
                f.cpu.fpr[5].bits[half_index] = test.f5[half_index];
            }
            f.cpu.fpscr = 0u;
        });
        for (Fixture* f : {native.get(), oracle.get()}) {
            if (f->cpu.fpr[test.d].bits[0] != test.expect[0] || f->cpu.fpr[test.d].bits[1] != test.expect[1]) {
                char row[200];
                std::snprintf(row, sizeof(row), "%s (%s): f%u = 0x%016llX 0x%016llX, expected 0x%016llX 0x%016llX",
                              test.program.name, f == native.get() ? "native" : "interpreter", test.d,
                              static_cast<unsigned long long>(f->cpu.fpr[test.d].bits[0]),
                              static_cast<unsigned long long>(f->cpu.fpr[test.d].bits[1]),
                              static_cast<unsigned long long>(test.expect[0]),
                              static_cast<unsigned long long>(test.expect[1]));
                require_that(false, row);
            }
        }
    }

    // psq_st f1 through GQR4..7 at r5+0x00/0x10/0x20/0x30, W=0 (ps0 then ps1).
    struct Saturation {
        u64 ps0, ps1;
        u16 u8v[2], u16v[2], s8v[2], s16v[2];
    };
    const Saturation saturations[] = {
        {kFpQNaN, 0xFFF0000000000000ull, {0xFF, 0x00}, {0xFFFF, 0x0000}, {0x7F, 0x80}, {0x7FFF, 0x8000}},
        {kFpNegQNaN, 0x7FF0000000000000ull, {0xFF, 0xFF}, {0xFFFF, 0xFFFF}, {0x7F, 0x7F}, {0x7FFF, 0x7FFF}},
        {kFpSNaN, double_to_bits(-2.9), {0xFF, 0x00}, {0xFFFF, 0x0000}, {0x7F, 0xFE}, {0x7FFF, 0xFFFE}},
    };
    for (const Saturation& test : saturations) {
        execute_both(code, GEKKO_SEM_PROGRAM(PsqStInteger), *native, *oracle, [&](Fixture& f) {
            f.cpu.gpr[5] = 0x80300200u;
            for (u32 index = 4; index <= 7u; ++index) f.cpu.spr[SPR_GQR0 + index] = index;  // scale 0
            f.cpu.fpr[1].bits[0] = test.ps0;
            f.cpu.fpr[1].bits[1] = test.ps1;
            for (u32 offset = 0; offset < 0x40u; ++offset) f.bus.write8(kSemData + 0x200u + offset, 0xAAu);
        });
        for (Fixture* f : {native.get(), oracle.get()}) {
            const u32 base = kSemData + 0x200u;
            const bool ok =
                f->bus.read8(base + 0x00u) == test.u8v[0] && f->bus.read8(base + 0x01u) == test.u8v[1] &&
                f->bus.read8(base + 0x02u) == 0xAAu &&
                f->bus.read16(base + 0x10u) == test.u16v[0] && f->bus.read16(base + 0x12u) == test.u16v[1] &&
                f->bus.read8(base + 0x14u) == 0xAAu &&
                f->bus.read8(base + 0x20u) == test.s8v[0] && f->bus.read8(base + 0x21u) == test.s8v[1] &&
                f->bus.read8(base + 0x22u) == 0xAAu &&
                f->bus.read16(base + 0x30u) == test.s16v[0] && f->bus.read16(base + 0x32u) == test.s16v[1] &&
                f->bus.read8(base + 0x34u) == 0xAAu;
            char row[200];
            std::snprintf(row, sizeof(row), "PsqStInteger (%s) ps0 0x%016llX ps1 0x%016llX: stored %02X %02X / %04X %04X / %02X %02X / %04X %04X",
                          f == native.get() ? "native" : "interpreter", static_cast<unsigned long long>(test.ps0),
                          static_cast<unsigned long long>(test.ps1), f->bus.read8(base), f->bus.read8(base + 1u),
                          f->bus.read16(base + 0x10u), f->bus.read16(base + 0x12u), f->bus.read8(base + 0x20u),
                          f->bus.read8(base + 0x21u), f->bus.read16(base + 0x30u), f->bus.read16(base + 0x32u));
            require_that(ok, row);
        }
    }

    // GQR0 type 0 with nonzero scale fields: the words move unchanged. GQR3 u16
    // with LD_SCALE -2 (F = I * 4), W=1 via psq_lu; GQR6 s16 with LD_SCALE 3.
    execute_both(code, GEKKO_SEM_PROGRAM(PsqGqr), *native, *oracle, [&](Fixture& f) {
        f.cpu.gpr[5] = 0x80300280u;
        f.cpu.spr[SPR_GQR0] = 0x3F003F00u;
        f.cpu.spr[SPR_GQR0 + 3] = (0x3Eu << 24) | (5u << 16);
        f.cpu.spr[SPR_GQR0 + 6] = (0x03u << 24) | (7u << 16);
        for (u32 reg = 1; reg <= 3u; ++reg) f.cpu.fpr[reg].bits[0] = f.cpu.fpr[reg].bits[1] = 0u;
        const u32 base = kSemData + 0x280u;
        f.bus.write32(base + 0x00u, 0x40490FDBu);
        f.bus.write32(base + 0x04u, 0xC1200000u);
        f.bus.write32(base + 0x08u, 0u);
        f.bus.write32(base + 0x0Cu, 0u);
        f.bus.write16(base + 0x10u, 0x0003u);
        f.bus.write16(base + 0x20u, 0xFFF0u);
        f.bus.write16(base + 0x22u, 0x0010u);
    });
    for (Fixture* f : {native.get(), oracle.get()}) {
        const Cpu& c = f->cpu;
        const u32 base = kSemData + 0x280u;
        require_that(c.fpr[1].bits[0] == double_to_bits(static_cast<double>(bits_to_float(0x40490FDBu))) &&
                         c.fpr[1].bits[1] == double_to_bits(-10.0),
                     "PsqGqr: psq_l type 0 must ignore LD_SCALE");
        require_that(f->bus.read32(base + 0x08u) == 0x40490FDBu && f->bus.read32(base + 0x0Cu) == 0xC1200000u,
                     "PsqGqr: psq_st type 0 must ignore ST_SCALE");
        require_that(c.fpr[2].bits[0] == double_to_bits(12.0) && c.fpr[2].bits[1] == double_to_bits(1.0) &&
                         c.gpr[5] == 0x80300290u,
                     "PsqGqr: psq_lu u16 3 * 2^2, W=1, update");
        require_that(c.fpr[3].bits[0] == double_to_bits(-2.0) && c.fpr[3].bits[1] == double_to_bits(2.0),
                     "PsqGqr: psq_l s16 -16 and 16 * 2^-3");
    }
    std::cout << "PASS generated FP compares (FPCC/VXSNAN/VXVC/FX, C preserved), paired moves, "
                 "quantised saturation and scales\n";
}

// ---- floating-point semantics (audit batch b7) ----
//
// fpu-1/-2/-3, paired-single-3: every arithmetic form the generator emits
// (FpSingles, FpDoubles, FpPaired; compiled here with the shards' /arch:AVX2,
// so the fused forms are the FMA instruction) runs natively and through the
// reference interpreter over operand classes and sparse random doubles, in both
// FPSCR[NI] modes, and must agree bit for bit; the audit scenarios are checked
// against their manual values. fpu-4 / paired-single-8: lfs/stfs/psq type-0
// conversions. paired-single-10: HID2 PSE/LSQE/LCE faults, including a guard
// the hoist must re-arm after mtspr 920.

constexpr u64 kFpSingleOne = 0x3FF0000000000000ull;

void run_generated(const std::string& name, RecompiledCode& code, Fixture& native);

u64 fp_single_word(u32 word) { return double_to_bits(static_cast<double>(bits_to_float(word))); }

void run_fp_program(RecompiledCode& code, const SemProgram& program, Fixture& native, Fixture& oracle,
                    u64 a, u64 b, u64 c, u32 fpscr) {
    execute_both(code, program, native, oracle, [&](Fixture& f) {
        for (u32 reg = 1; reg <= 17u; ++reg) f.cpu.fpr[reg].bits[0] = f.cpu.fpr[reg].bits[1] = 0x5555u;
        f.cpu.fpr[1].bits[0] = a;
        f.cpu.fpr[1].bits[1] = c;  // the paired halves differ: ps1 gets (C, A, B)
        f.cpu.fpr[2].bits[0] = b;
        f.cpu.fpr[2].bits[1] = a;
        f.cpu.fpr[3].bits[0] = c;
        f.cpu.fpr[3].bits[1] = b;
        f.cpu.fpscr = fpscr;
    });
}

void fp_semantics(RecompiledCode& code) {
    auto native = std::make_unique<Fixture>();
    auto oracle = std::make_unique<Fixture>();
    const SemProgram programs[] = {GEKKO_SEM_PROGRAM(FpSingles), GEKKO_SEM_PROGRAM(FpDoubles),
                                   GEKKO_SEM_PROGRAM(FpPaired)};
    const u64 classes[] = {
        0x0000000000000000ull, 0x8000000000000000ull, 0x0000000000000001ull, 0x800FFFFFFFFFFFFFull,
        0x0010000000000000ull, 0x36A0000000000000ull, 0x3810000000000000ull, 0x3820000000000000ull,
        kFpSingleOne,          0xBFF8000000000000ull, 0x3FF0000002000000ull, 0x3FB999999999999Aull,
        0x47EFFFFFE0000000ull, 0x7FEFFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
        kFpQNaN,               kFpNegQNaN,            kFpSNaN,               0xFFF0000000000001ull,
        fp_single_word(0x397FFFC0u), fp_single_word(0x39800020u), fp_single_word(0x3F800001u),
        0x41E0000000000000ull, 0x41DFFFFFFFE00000ull,
    };
    std::mt19937_64 random(0xb7fau);
    const auto draw = [&]() -> u64 {
        if ((random() & 1u) == 0u) return classes[random() % (sizeof(classes) / sizeof(classes[0]))];
        u64 mantissa = random() & 0x000fffffe0000000ull;
        for (u64 bit = random() % 3u; bit != 0u; --bit) mantissa |= 1ull << (random() % 52u);
        const u64 exponent = (random() & 3u) == 0u ? random() % 2048u : 1023u - 70u + random() % 140u;
        return ((random() & 1u) << 63) | (exponent << 52) | mantissa;
    };
    unsigned cases = 0;
    for (const SemProgram& program : programs) {
        for (u32 fpscr : {0u, kFpscrNI}) {
            for (unsigned iteration = 0; iteration < 1500u; ++iteration) {
                const u64 a = draw(), b = draw(), c = draw();
                try {
                    run_fp_program(code, program, *native, *oracle, a, b, c, fpscr);
                } catch (const std::exception& error) {
                    char row[256];
                    std::snprintf(row, sizeof(row), " (A 0x%016llX B 0x%016llX C 0x%016llX FPSCR 0x%08X)",
                                  static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
                                  static_cast<unsigned long long>(c), fpscr);
                    throw std::runtime_error(std::string(error.what()) + row);
                }
                ++cases;
            }
        }
    }

    // The audit scenarios, against their manual values (both engines agree by
    // now; the value must be the manual's).
    const auto expect = [&](const char* name, u32 reg, u64 value) {
        for (Fixture* f : {native.get(), oracle.get()}) {
            if (f->cpu.fpr[reg].bits[0] != value) {
                char row[200];
                std::snprintf(row, sizeof(row), "%s (%s): f%u = 0x%016llX, expected 0x%016llX", name,
                              f == native.get() ? "native" : "interpreter", reg,
                              static_cast<unsigned long long>(f->cpu.fpr[reg].bits[0]),
                              static_cast<unsigned long long>(value));
                require_that(false, row);
            }
        }
    };
    // fpu-2: fmsub f11 = f1*f3 - f2, f1 = f3 = 1+2^-27, f2 = 1: one rounding.
    run_fp_program(code, programs[1], *native, *oracle, 0x3FF0000002000000ull, kFpSingleOne,
                   0x3FF0000002000000ull, 0u);
    expect("fmsub fused (fpu-2)", 11, 0x3E50000001000000ull);
    // paired-single-3: ps_madd f10 = f1*f3 + f2 lands on a single midpoint.
    run_fp_program(code, programs[2], *native, *oracle, fp_single_word(0x397FFFC0u),
                   fp_single_word(0x3F800001u), fp_single_word(0x39800020u), 0u);
    expect("ps_madd one rounding (paired-single-3)", 10, fp_single_word(0x3F800001u));
    // fpu-3: fdivs 0/0 generates the positive default QNaN; fnmsub of a QNaN
    // operand keeps its sign.
    run_fp_program(code, programs[0], *native, *oracle, 0u, 0u, kFpSingleOne, 0u);
    expect("fdivs 0/0 default QNaN (fpu-3)", 6, kFpQNaN);
    run_fp_program(code, programs[1], *native, *oracle, kFpQNaN, 0u, kFpSingleOne, 0u);
    expect("fnmsub QNaN sign (fpu-3)", 13, kFpQNaN);
    expect("fnmadd QNaN sign (fpu-3)", 12, kFpQNaN);
    // fpu-1: fmuls 1e-20f * 1e-20f is a single denormal: zero in non-IEEE mode.
    const u64 tiny = fp_single_word(0x1E3CE508u);  // 1e-20f
    run_fp_program(code, programs[0], *native, *oracle, tiny, kFpSingleOne, tiny, kFpscrNI);
    expect("fmuls denormal result with NI (fpu-1)", 7, 0u);
    run_fp_program(code, programs[0], *native, *oracle, tiny, kFpSingleOne, tiny, 0u);
    expect("fmuls denormal result IEEE", 7, 0x37A16C2000000000ull);
    // fpu-1: fctiw of 2^31 in non-IEEE mode.
    run_fp_program(code, programs[1], *native, *oracle, 0u, 0x41E0000000000000ull, 0u, kFpscrNI);
    expect("fctiw 2^31 with NI (fpu-1)", 15, 0xFFF8000080000000ull);

    // fpu-4 / paired-single-8: an SNaN and a denormal through lfs, stfs, lfsx,
    // psq_st/psq_l type 0 and stfsux.
    const SemProgram conversions = GEKKO_SEM_PROGRAM(FpConversions);
    struct Conversion {
        u32 word;
        u32 stfs, psq_first;
    };
    const Conversion rows[] = {
        {0x7F800001u, 0x7F800001u, 0x7F800001u},  // SNaN survives every path
        {0x000116C2u, 0x000116C2u, 0x00000000u},  // denormal: stfs keeps, psq_st stores 0
        {0x800116C2u, 0x800116C2u, 0x80000000u},
        {0x3F800000u, 0x3F800000u, 0x3F800000u},
    };
    for (const Conversion& row : rows) {
        execute_both(code, conversions, *native, *oracle, [&](Fixture& f) {
            f.cpu.gpr[5] = 0x80300300u;
            f.cpu.gpr[6] = 0x4u;
            f.cpu.gpr[7] = 0x20u;
            for (u32 offset = 0; offset < 0x40u; offset += 4u) f.bus.write32(kSemData + 0x300u + offset, 0xAAAAAAAAu);
            f.bus.write32(kSemData + 0x300u, row.word);
            f.bus.write32(kSemData + 0x304u, row.word);
            f.cpu.spr[SPR_GQR0] = 0u;
        });
        const u64 loaded = single_to_double_bits(row.word);
        for (Fixture* f : {native.get(), oracle.get()}) {
            const Cpu& c = f->cpu;
            const u32 base = kSemData + 0x300u;
            const bool ok = c.fpr[1].bits[0] == loaded && c.fpr[1].bits[1] == loaded && c.fpr[2].bits[0] == loaded &&
                            f->bus.read32(base + 0x08u) == row.stfs && f->bus.read32(base + 0x10u) == row.psq_first &&
                            f->bus.read32(base + 0x14u) == row.psq_first &&
                            c.fpr[3].bits[0] == single_to_double_bits(row.psq_first) &&
                            f->bus.read32(base + 0x20u) == row.stfs && c.gpr[5] == 0x80300320u;
            char text[160];
            std::snprintf(text, sizeof(text), "FpConversions (%s) word 0x%08X", f == native.get() ? "native" : "interpreter",
                          row.word);
            require_that(ok, text);
        }
    }
    require_that(single_to_double_bits(0x7F800001u) == 0x7FF0000020000000ull, "lfs of an SNaN must not quiet it");

    // paired-single-10: faults. Native runs the generated function, the oracle
    // steps; both must end at the program (or FP-unavailable) handler with the
    // faulting instruction not executed (the interpreter does not count it as
    // completed) and its address saved as SRR0 in the OSContext.
    struct Fault {
        SemProgram program;
        u32 hid2;
        bool fp;
        u32 r8;
        u32 completed;  // instructions before the fault
        u32 handler;
    };
    // Change C: LSQE|PSE are machine invariants, so every HID2 below keeps
    // them; LCE (dcbz_l) and MSR[FP] are the run-time conditions left.
    const Fault faults[] = {
        {GEKKO_SEM_PROGRAM(Hid2Dcbzl), 0xE0000000u, true, 0u, 0u, kProgramHandler},
        {GEKKO_SEM_PROGRAM(Hid2Dcbzl), 0xE0000000u, false, 0u, 0u, kProgramHandler},
        {GEKKO_SEM_PROGRAM(Hid2Paired), 0xA0000000u, false, 0u, 0u, kFpHandler},
        {GEKKO_SEM_PROGRAM(Hid2PsqL), 0xE0000000u, false, 0u, 0u, kFpHandler},
    };
    for (const Fault& fault : faults) {
        for (Fixture* f : {native.get(), oracle.get()}) {
            f->cpu.reset();
            for (std::size_t index = 0; index < fault.program.count; ++index) {
                f->bus.write32((fault.program.address) + 4u * static_cast<u32>(index),
                               fault.program.words[index]);
            }
            f->cpu.set_entry(fault.program.address);
            f->cpu.lr = kSemReturn;
            f->cpu.spr[SPR_HID2] = fault.hid2;
            if (!fault.fp) f->cpu.set_msr(f->cpu.msr() & (~kMsrFP));
            f->cpu.gpr[5] = 0x80300300u;
            f->cpu.gpr[8] = fault.r8;
            f->bus.write32(kSemData + 0x300u, 0x12345678u);
        }
        run_generated(fault.program.name, code, *native);
        // The instructions before the fault, then the faulting one, which completes nothing.
        require_that(step_until(*oracle, fault.handler, fault.completed + 1u) == fault.completed + 1u,
                     std::string(fault.program.name) + ": the interpreter did not fault on the native path");
        const std::string name = std::string(fault.program.name) + " HID2 0x" + std::to_string(fault.hid2);
        compare(native->cpu, oracle->cpu);
        for (Fixture* f : {native.get(), oracle.get()}) {
            const Cpu& c = f->cpu;
            // Both machines were reset for this case: the interpreter completed exactly the instructions
            // before the fault, and native code interprets nothing.
            const u64 completed = f == oracle.get() ? fault.completed : 0u;
            require_that(c.pc == fault.handler && c.interpreted_instructions() == completed &&
                             saved_srr0(*f) == fault.program.address + 4u * fault.completed &&
                             ((saved_srr1(*f) & kSrr1IllegalInstruction) != 0u) ==
                                 (fault.handler == kProgramHandler) &&
                             f->bus.read32(kSemData + 0x300u) == 0x12345678u,
                         name + (f == native.get() ? ": native" : ": interpreter") + " fault state");
        }
    }
    // An mtspr HID2 that would clear LSQE|PSE is refused by both engines: the
    // machine stops and HID2 keeps its value. One that keeps them executes.
    // (Translated code reaches its next boundary before the stop is observed;
    // the interpreter stops after the mtspr - both refuse the write.)
    for (const u32 value : {0xC0000000u, 0xE0000000u}) {
        const bool refused = (value & kHid2PairedSingleEnables) != kHid2PairedSingleEnables;
        const SemProgram program = GEKKO_SEM_PROGRAM(Hid2Mtspr);
        for (Fixture* f : {native.get(), oracle.get()}) {
            f->cpu.reset();
            for (std::size_t index = 0; index < program.count; ++index) {
                f->bus.write32((program.address) + 4u * static_cast<u32>(index),
                               program.words[index]);
            }
            f->cpu.set_entry(program.address);
            f->cpu.lr = kSemReturn;
            f->cpu.gpr[8] = value;
        }
        run_generated("Hid2Mtspr", code, *native);
        step_until(*oracle, kSemReturn, 16u);
        for (Fixture* f : {native.get(), oracle.get()}) {
            const Cpu& c = f->cpu;
            require_that(c.stopped() == refused && c.spr[SPR_HID2] == (refused ? 0xE0000000u : value),
                         std::string("Hid2Mtspr 0x") + std::to_string(value) +
                             (f == native.get() ? ": native" : ": interpreter") +
                             (refused ? " must refuse the write" : " must accept the write"));
        }
        if (!refused) compare(native->cpu, oracle->cpu);
    }

    // The enabled forms execute natively: dcbz_l with LCE, psq_lx.
    // (The faults above left both machines at a handler with MSR[FP] clear.)
    execute_both(code, GEKKO_SEM_PROGRAM(Hid2Dcbzl), *native, *oracle, [&](Fixture& f) {
        f.cpu.reset();
        f.cpu.spr[SPR_HID2] = 0xF0000000u;
        f.cpu.gpr[5] = 0x80300300u;
        f.bus.write32(kSemData + 0x300u, 0x12345678u);
    });
    require_that(native->bus.read32(kSemData + 0x300u) == 0u, "dcbz_l with LCE zeroes the block");
    execute_both(code, GEKKO_SEM_PROGRAM(Hid2PsqLx), *native, *oracle, [&](Fixture& f) {
        f.cpu.reset();
        f.cpu.spr[SPR_HID2] = 0xA0000000u;
        f.cpu.spr[SPR_GQR0] = 0u;
        f.cpu.gpr[5] = 0x80300300u;
        f.cpu.gpr[6] = 0u;
        f.bus.write32(kSemData + 0x300u, 0x3F800000u);
    });
    require_that(native->cpu.fpr[1].bits[0] == kFpSingleOne, "psq_lx loads with the paired-single enables");

    std::cout << "PASS generated FP arithmetic == reference over " << cases
              << " operand sets (both NI modes), audit scenarios, conversions, HID2 faults\n";
}

// ---- jump-table loops (audit finding branch-sys-1) ----
//
// kBctrLoop has fn_803E1D58's shape: a computed bctr picks one of two case
// rows, both absorbed into the loop's row, and each case branches back to the
// loop head. bctr is a plain jump (NIA <- CTR), so one guest iteration must
// not leave host stack behind. The generator used to lower it to
// recomp::jump_indirect, a real call into the case row's thunk, which
// forwarded into the cold body, whose back edge called the hot body again:
// several host frames per iteration until the guest returned. The host stack
// pointer is sampled where every iteration passes - the bctr into a case
// row's own entry runs that row's try_hle, so a declining hook on each row
// probes it (the per-retire_block hardware tick that sampled it went with the
// instruction clock) - and the spread must not scale with the iteration
// count. The bctr is also the loop's only backward transfer, so it must poll
// needs_dispatch (with a stop pending it yields at every iteration), and a
// jump to a function row's own entry must still pass that entry's try_hle
// (HLE hooks, --verify-recomp cuts).

constexpr u32 kBctrReturn = 0x80009000u;
constexpr u32 kBctrIterations = 1500u;

void load_bctr_loop(Fixture& f, u32 entry) {
    for (std::size_t index = 0; index < sizeof(kBctrLoopWords) / sizeof(kBctrLoopWords[0]); ++index) {
        f.bus.write32((kBctrLoopAddress) + 4u * static_cast<u32>(index),
                      kBctrLoopWords[index]);
    }
    f.bus.write32(((kBctrLoopAddress + 0x44u)), 0x4E800020u);
    f.cpu.set_entry(entry);
    f.cpu.lr = kBctrReturn;
    f.cpu.gpr[3] = 0u;
    f.cpu.gpr[4] = 0u;
    f.cpu.gpr[5] = kBctrIterations;
    f.cpu.gpr[8] = kBctrLoopAddress + 0x18u;  // what the entry path loads
}

void indirect_jump_loop(RecompiledCode& code) {
    require_that(code.lookup(kBctrLoopAddress) != nullptr &&
                     code.lookup(kBctrLoopAddress + 0x18u) != nullptr &&
                     code.lookup(kBctrLoopAddress + 0x1Cu) != nullptr,
                 "bctr loop: the generator did not translate the loop and both case rows");
    u32 loop_r4 = 0;
    u64 loop_instructions = 0;
    for (u32 i = 1; i <= kBctrIterations; ++i) {
        loop_r4 += (i & 1u) != 0u ? 2u : 3u;             // odd -> case2, even -> case1+case2
        loop_instructions += (i & 1u) != 0u ? 9u : 10u;  // 7 head + 2 or 3 case
    }
    // instructions: the route's whole path - the reference interpreter must reach the same return over
    // exactly these, and the verified replays must compare every one of them.
    struct Route {
        const char* name;
        u32 offset;
        u32 r4;
        u64 instructions;
    };
    const Route routes[] = {
        {"function entry", 0x00u, loop_r4, 6u + loop_instructions + 3u},
        {"cold interior entry", 0x20u, loop_r4, 1u + loop_instructions + 3u},
        {"absorbed case-row entry", 0x1Cu, 2u + loop_r4, 2u + loop_instructions + 3u},
    };
    // Whole: one dispatch, nothing hooked. StopPending: a host stop is pending at every dispatch - a
    // pause hook on every address a run can start at requests it and declines - so the bctr must yield
    // at every iteration (what a one-instruction budget made it do before budgets were deleted).
    // Verified: --verify-recomp. Hooked: declining hooks on both case rows, which sample the host stack
    // at every iteration (every route: the stack check the whole-budget runs made by hardware tick).
    enum class Mode { Whole, StopPending, Verified, Hooked };
    unsigned cases = 0;
    for (const Route& route : routes)
        for (Mode mode : {Mode::Whole, Mode::StopPending, Mode::Verified, Mode::Hooked}) {
            const std::string name = std::string("bctr loop, ") + route.name + ", mode " +
                                     std::to_string(static_cast<int>(mode));
            auto native = std::make_unique<Fixture>();
            auto oracle = std::make_unique<Fixture>();
            const u32 entry = kBctrLoopAddress + route.offset;
            load_bctr_loop(*native, entry);
            load_bctr_loop(*oracle, entry);
            native->cpu.attach_recompiled(&code);
            oracle->cpu.attach_recompiled(nullptr);
            std::uintptr_t stack_low = UINTPTR_MAX;
            std::uintptr_t stack_high = 0;
            u64 samples = 0;
            u64 case1_hits = 0, case2_hits = 0, entry_pauses = 0;
            // Every hook declines, so the guest result is unchanged. Installed on the native machine
            // only: the interpreter runs a hook on every arrival, fall-through too.
            const auto probe = [&](Cpu& c, u64& hits) {
                volatile char marker = 0;
                const std::uintptr_t sp = reinterpret_cast<std::uintptr_t>(&marker);
                stack_low = std::min(stack_low, sp);
                stack_high = std::max(stack_high, sp);
                ++samples;
                ++hits;
                if (mode == Mode::StopPending) c.request_stop(StopReason::HostRequest, "bctr loop pause");
                return false;
            };
            if (mode == Mode::StopPending || mode == Mode::Hooked) {
                native->cpu.install_hle(kBctrLoopAddress + 0x18u, [&](Cpu& c) { return probe(c, case1_hits); },
                                        "bctr loop case 1 hook");
                native->cpu.install_hle(kBctrLoopAddress + 0x1Cu, [&](Cpu& c) { return probe(c, case2_hits); },
                                        "bctr loop case 2 hook");
            }
            if (mode == Mode::StopPending && route.offset != 0x1Cu) {
                native->cpu.install_hle(entry, [&](Cpu& c) { return probe(c, entry_pauses); },
                                        "bctr loop entry pause");
            }
            if (mode == Mode::Verified) native->cpu.set_verify_recompiled(true);
            const GameTime time(native->cpu);
            const u64 interpreted = g_non_native_ppc_executions.load();
            u64 dispatches = 0;
            for (u64 runs = 0; !native->cpu.stopped() && native->cpu.pc != kBctrReturn &&
                               runs <= 4u * kBctrIterations;
                 ++runs) {
                dispatches += native->cpu.run_steps(1u);
                if (mode == Mode::StopPending && native->cpu.stop().detail == "bctr loop pause") {
                    native->cpu.clear_stop();
                }
            }
            if (mode != Mode::Verified) {
                require_that(g_non_native_ppc_executions.load() == interpreted &&
                                 native->cpu.interpreted_instructions() == 0u,
                             name + ": native run used the interpreter");
            }
            require_that(!native->cpu.stopped() && native->cpu.pc == kBctrReturn,
                         name + ": native run did not return: " + native->cpu.stop().detail);
            require_that(native->cpu.gpr[3] == kBctrIterations && native->cpu.gpr[4] == route.r4,
                         name + ": wrong loop result");
            require_that(step_until(*oracle, kBctrReturn, route.instructions + 1u) == route.instructions &&
                             !oracle->cpu.stopped() && oracle->cpu.pc == kBctrReturn,
                         name + ": interpreter did not return over the route's instructions");
            compare(native->cpu, oracle->cpu);
            require_that(GameTime(native->cpu) == time, name + ": the loop moved game time");
            const std::uintptr_t spread = samples != 0u ? stack_high - stack_low : 0u;
            char detail[160];
            std::snprintf(detail, sizeof(detail), ": %llu dispatches, %llu stack samples, spread %llu bytes",
                          static_cast<unsigned long long>(dispatches),
                          static_cast<unsigned long long>(samples),
                          static_cast<unsigned long long>(spread));
            if (mode == Mode::StopPending || mode == Mode::Hooked) {
                require_that(samples >= kBctrIterations,
                             name + ": too few stack samples" + detail);
                // A few frames of spread (cold entry vs hot body) is expected;
                // one frame per iteration is well over 16 KiB at 1500 iterations.
                require_that(spread < 16u * 1024u,
                             name + ": host stack grew with guest iterations" + detail);
            }
            switch (mode) {
                case Mode::Whole:
                case Mode::Hooked:
                    require_that(dispatches == 1u && native->cpu.interpreter_exits == 0u,
                                 name + ": whole-loop run left native code" + detail);
                    break;
                case Mode::StopPending:
                    // The bctr is the only backward transfer: it must yield
                    // once per iteration, the first time after the entry path.
                    require_that(dispatches == kBctrIterations + 1u &&
                                     entry_pauses == (route.offset != 0x1Cu ? 1u : 0u),
                                 name + ": bctr back edge did not poll needs_dispatch" + detail);
                    break;
                case Mode::Verified: {
                    const auto& report = native->cpu.verify_report();
                    char figures[224];
                    std::snprintf(figures, sizeof(figures),
                                  "; verified %llu (%llu instructions of %llu), mismatches %llu, skipped exit %llu "
                                  "device %llu",
                                  static_cast<unsigned long long>(report.verified),
                                  static_cast<unsigned long long>(report.verified_instructions),
                                  static_cast<unsigned long long>(route.instructions),
                                  static_cast<unsigned long long>(report.mismatches),
                                  static_cast<unsigned long long>(report.skipped_exit),
                                  static_cast<unsigned long long>(report.skipped_device));
                    // Every jump into a case ROW's own entry is a call cut, so
                    // each iteration is a verified segment of its own, and the
                    // segments' replays together compare the whole route.
                    require_that(report.mismatches == 0u && report.verified >= kBctrIterations,
                                 name + ": verification mismatch or the case-row try_hle cut was lost" +
                                     detail + figures);
                    require_that(report.skipped_exit == 0u && report.skipped_device == 0u &&
                                     report.verified_instructions == route.instructions,
                                 name + ": the replays must compare every instruction of the route" + detail +
                                     figures);
                    break;
                }
            }
            if (mode == Mode::Hooked) {
                // Odd iterations jump to case 2, even ones to case 1; an entry
                // AT a hooked row runs that row's hook once more, from the
                // dispatcher.
                require_that(case2_hits == (kBctrIterations + 1u) / 2u + (route.offset == 0x1Cu ? 1u : 0u) &&
                                 case1_hits == kBctrIterations / 2u,
                             name + ": a bctr into a hooked function entry must run its hook");
            }
            std::cout << "  " << name << detail << '\n';
            ++cases;
        }
    std::cout << "PASS " << cases
              << " generated jump-table loop runs (flat host stack, back-edge poll, try_hle choke point, "
                 "cold and absorbed-row entries, verified)\n";
}

// Shared computed-jump dispatch must preserve site-specific direction, unknown
// targets, conditional fallthrough and cold entry. The interpreter is the oracle.
void shared_indirect_jumps(RecompiledCode& code) {
    const auto setup = [&](Fixture& f, u32 entry, u32 target, bool equal) {
        for (unsigned i = 0; i < sizeof(kMultiBctrWords) / sizeof(kMultiBctrWords[0]); ++i)
            f.bus.write32(kMultiBctrAddress + 4u * i, kMultiBctrWords[i]);
        f.bus.write32(0x80008000u, 0x38630001u);
        f.bus.write32(0x80008004u, 0x38840001u);
        f.bus.write32(0x80008008u, 0x4E800020u);
        f.cpu.set_entry(kMultiBctrAddress + entry);
        f.cpu.lr = kBctrReturn;
        f.cpu.gpr[3] = 0u;
        f.cpu.gpr[4] = 0u;
        f.cpu.gpr[8] = target;
        f.cpu.gpr[9] = kMultiBctrAddress + 0x14u;
        f.cpu.ctr = target;
        f.cpu.cr = equal ? 0x20000000u : 0u;
    };
    struct Route { u32 entry, target; bool equal; u32 result; u64 instructions; };
    const Route routes[] = {
        {0, kMultiBctrAddress + 8u, true, 3, 9},
        {0, kMultiBctrAddress + 11u, true, 3, 9}, // CTR low bits ignored
        {0, kMultiBctrAddress + 12u, true, 2, 8}, // unknown local interior
        {0, 0x80008000u, true, 1, 7},            // external target
        {0x24, kMultiBctrAddress + 8u, false, 6, 5},
        {0x24, kMultiBctrAddress + 8u, true, 3, 6},
        {0x10, kMultiBctrAddress + 0x17u, true, 2, 3},
    };
    for (const auto& route : routes) {
        auto native = std::make_unique<Fixture>();
        auto oracle = std::make_unique<Fixture>();
        setup(*native, route.entry, route.target, route.equal);
        setup(*oracle, route.entry, route.target, route.equal);
        native->cpu.attach_recompiled(&code);
        const u64 before = g_non_native_ppc_executions.load();
        for (unsigned i = 0; i < 8 && native->cpu.pc != kBctrReturn && !native->cpu.stopped(); ++i)
            native->cpu.run_steps(1u);
        require(native->cpu.pc == kBctrReturn && !native->cpu.stopped(), "shared bctr route did not return");
        require(native->cpu.gpr[4] == route.result, "shared bctr wrong result");
        require(g_non_native_ppc_executions.load() == before, "shared bctr used interpreter");
        require(step_until(*oracle, kBctrReturn, 20u) == route.instructions, "shared bctr oracle route changed");
        compare(native->cpu, oracle->cpu);
    }
    // Pending stop at the first site must yield with that site's address, not
    // the address of another computed jump sharing the dispatch block.
    {
        auto native = std::make_unique<Fixture>();
        auto oracle = std::make_unique<Fixture>();
        setup(*native, 0, kMultiBctrAddress + 8u, true);
        setup(*oracle, 0, kMultiBctrAddress + 8u, true);
        native->cpu.attach_recompiled(&code);
        native->cpu.set_record_fallbacks(true);
        native->cpu.install_hle(kMultiBctrAddress, [](Cpu& c) {
            c.request_stop(StopReason::HostRequest, "shared bctr pause"); return false;
        }, "shared bctr pause");
        native->cpu.run_steps(1u);
        require(native->cpu.pc == kMultiBctrAddress + 8u, "shared bctr did not yield at backward target");
        require(step_until(*oracle, native->cpu.pc, 10u) == 4u, "shared bctr yield oracle route changed");
        compare(native->cpu, oracle->cpu);
        const auto sites = native->cpu.fallback_sites();
        require(sites.size() == 1u && sites[0].caller == kMultiBctrAddress + 0x24u &&
                    sites[0].target == kMultiBctrAddress + 8u && sites[0].reason == FallbackReason::Yield,
                "shared bctr lost actual branch site");
    }
    // A stop first requested at case 1 must not make the forward bctr poll.
    for (bool served : {false, true}) {
        auto native = std::make_unique<Fixture>();
        setup(*native, 8u, kMultiBctrAddress + 8u, true);
        native->cpu.attach_recompiled(&code);
        native->cpu.install_hle(kMultiBctrAddress + 8u, [&](Cpu& c) {
            if (!served) c.request_stop(StopReason::HostRequest, "forward bctr pause"); return false;
        }, "forward bctr pause");
        unsigned hits = 0;
        native->cpu.install_hle(kMultiBctrAddress + 0x14u, [&](Cpu&) { ++hits; return served; }, "shared target");
        native->cpu.run_steps(1u);
        require(native->cpu.pc == kBctrReturn && native->cpu.gpr[4] == (served ? 1u : 3u),
                "shared bctr forward target/hook result wrong");
        require(hits == 1u, "shared bctr target hook must run exactly once");
        require(native->cpu.fallback_total(FallbackReason::Yield) == 0u, "shared bctr polled on a forward jump");
    }
    std::cout << "PASS shared computed jumps (7 oracle routes, actual-site yield, forward poll exclusion, target hooks)\n";
}

// ---- asynchronous interrupt recognition (audit batch b5) ----
//
// Each program is translated by ppcrecomp and run natively (the generated
// function called directly, as Cpu::run enters it) and through the reference
// interpreter from identical state. Both engines must stop at the same
// recognition point (cpu.h, interrupt_lines_) with the condition still latched,
// and the dispatcher must then take it with SRR0 at exactly that point. The
// exception entry is the native OS entry; the handlers the fixture installs
// have no translated code, so the reference dispatcher interprets their
// `li r31,<vector>` marker.
//
// Under native game time no condition can arise from guest code passing time:
// the decrementer edge is a game-clock event the scheduler delivers at a wait
// point, and the hardware tick that used to raise conditions mid-block is gone
// with the instruction clock. So a condition is delivered before the code is
// entered (the generated function is then entered the way the dispatcher
// enters a handed-off callee, with no R1 in front of it), or raised from inside
// a call by a declining hook - a host effect at a call boundary - and the
// instructions each engine executes before the recognition point are pinned
// by the interpreter's step count and the architected results.

constexpr u32 kIrqReturn = 0x80009000u;
constexpr u32 kIrqFlag = 0x80600000u;  // in the program image, past the DOL's sections (no game function)

struct IrqProgram {
    const char* name;
    u32 address;
    const unsigned* words;
    std::size_t count;
};
#define GEKKO_IRQ_PROGRAM(Name)                                    \
    IrqProgram{#Name, kIrq##Name##Address, kIrq##Name##Words,       \
               sizeof(kIrq##Name##Words) / sizeof(kIrq##Name##Words[0])}

void load_irq_program(Fixture& f, const IrqProgram& program) {
    for (std::size_t index = 0; index < program.count; ++index) {
        f.bus.write32((program.address) + 4u * static_cast<u32>(index),
                      program.words[index]);
    }
    f.cpu.set_entry(program.address);
    f.cpu.lr = kIrqReturn;
}

void compare_irq(const std::string& name, Fixture& native, Fixture& oracle) {
    try {
        compare(native.cpu, oracle.cpu);
    } catch (const std::exception& error) {
        throw std::runtime_error(name + ": " + error.what());
    }
    require_that(native.cpu.external_interrupt() == oracle.cpu.external_interrupt() &&
                     native.cpu.decrementer_pending() == oracle.cpu.decrementer_pending(),
                 name + ": the engines latched different interrupt conditions");
}

// Steps the reference interpreter over `count` instructions - the native run's
// path to its recognition point - each of which must complete.
void step_to(const std::string& name, Fixture& oracle, u64 count) {
    const u64 completed = oracle.cpu.interpreted_instructions();
    for (u64 index = 0; index < count && !oracle.cpu.stopped(); ++index) oracle.cpu.step();
    require_that(!oracle.cpu.stopped() && oracle.cpu.interpreted_instructions() - completed == count,
                 name + ": the interpreter did not complete the native run's instructions");
}

// The scheduler's delivery of the decrementer's 0 -> 1 edge
// (System::advance_game_time, at a wait point): the clock reaches the edge's
// deadline, where DEC reads 0xFFFFFFFF, and the condition latches.
void deliver_decrementer_edge(Cpu& cpu) {
    cpu.game_clock().set_now(cpu.game_clock().deadline(GameEvent::Decrementer));
    cpu.latch_decrementer();
}

// Runs the generated function the way Cpu::run enters it - directly, with no
// recognition point in front of it, as for a handed-off callee when a
// condition is already deliverable - and requires that no instruction was
// interpreted.
void run_generated(const std::string& name, RecompiledCode& code, Fixture& native) {
    const RecompiledCode::Fn fn = code.lookup(native.cpu.pc);
    require_that(fn != nullptr, name + ": the generator did not translate the program");
    native.cpu.attach_recompiled(&code);
    const u64 interpreted = g_non_native_ppc_executions.load();
    const u64 completed = native.cpu.interpreted_instructions();
    fn(native.cpu);
    require_that(g_non_native_ppc_executions.load() == interpreted &&
                     native.cpu.interpreted_instructions() == completed,
                 name + ": native run used the interpreter");
}

// One engine takes the latched condition: the native machine through Cpu::run's
// R1 check, the oracle through step(), which may only take it because the
// instruction it stopped after is a recognition point. Either way the OS entry
// saves SRR0 / SRR1 in the OSContext and exactly one instruction - the
// handler's marker - is then interpreted.
void take_interrupt(const std::string& name, Fixture& f, bool native, u32 vector, u32 srr0,
                    bool srr1_ee) {
    const u32 handler = vector == 0x900u ? kDecrementerHandler : kExternalHandler;
    const u64 before = f.cpu.interpreted_instructions();
    if (native) {
        require_that(f.cpu.run_steps(1u) == 1u, name + ": the dispatcher must take it in one dispatch");
    } else {
        f.cpu.step();
    }
    const Cpu& c = f.cpu;
    require_that(!c.stopped() && c.interpreted_instructions() == before + 1u && c.gpr[31] == vector &&
                     c.pc == handler + 4u && saved_srr0(f) == srr0 &&
                     ((saved_srr1(f) & kMsrEE) != 0u) == srr1_ee && (c.msr() & kMsrEE) == 0u,
                 name + (native ? ": dispatcher" : ": interpreter") +
                     " did not take the interrupt at the recognition point");
    if (vector == 0x900u) {
        require_that(!c.decrementer_pending(), name + ": taking 0x900 must consume the condition");
    }
}

// dispatch-exceptions-2 / oracle-reference-5: the OS idle loop shape
// (`lwz; cmplwi 0; beq self`) with MSR[EE] = 1 and the decrementer's edge
// delivered. The loop's back edge must hand back at its first iteration - the
// latch used to be invisible to needs_dispatch, so the loop ran on until an
// unrelated external interrupt - and the interpreter must stop at the same
// boundary without having raised 0x900 early, in plain and verified runs. An
// access trace counts the loop's flag reads: its iterations (the count
// the instruction clock and the coalesce clamp used to give), and the guard
// that stops a loop that never takes the condition.
void decrementer_idle_loop(RecompiledCode& code) {
    const IrqProgram poll{"PollStable", kPollStableAddress, kPollStableWords,
                          sizeof(kPollStableWords) / sizeof(kPollStableWords[0])};
    for (bool verified : {false, true}) {
        const std::string name =
            std::string("decrementer edge at a generated idle loop") + (verified ? " (verified)" : "");
        auto native = std::make_unique<Fixture>();
        auto oracle = std::make_unique<Fixture>();
        for (Fixture* f : {native.get(), oracle.get()}) {
            load_irq_program(*f, poll);
            f->bus.write32(kIrqFlag, 0u);
            f->cpu.gpr[4] = kIrqFlag;
            f->cpu.set_msr(f->cpu.msr() | (kMsrEE));
            f->cpu.set_decrementer(0u);
            deliver_decrementer_edge(f->cpu);
        }
        u64 iterations = 0;
        Cpu* const cpu = &native->cpu;
        native->bus.set_access_trace([&iterations, cpu](u32 address, std::size_t, bool writing) {
            if (writing || address != kIrqFlag) return;
            if (++iterations >= 100000u) {
                cpu->request_stop(StopReason::HostRequest, "idle loop never took the decrementer");
            }
        });
        oracle->cpu.attach_recompiled(nullptr);
        const GameTime time(native->cpu);
        if (!verified) {
            run_generated(name, code, *native);
            const Cpu& n = native->cpu;
            require_that(!n.stopped() && n.exiting() && n.pc == kPollStableAddress && iterations == 1u &&
                             n.decrementer_pending() && n.decrementer() == 0xFFFFFFFFu &&
                             GameTime(n) == time,
                         name + ": the back edge of the first iteration must hand back (" +
                             std::to_string(iterations) + " iterations, " + n.stop().detail + ")");
            step_to(name, *oracle, 3u);  // lwz, cmplwi, the taken beq
            compare_irq(name, *native, *oracle);
            take_interrupt(name, *native, true, 0x900u, kPollStableAddress, true);
        } else {
            // Through the dispatcher, entered as a handed-off callee (no R1 in
            // front of it) so the loop meets the condition at its own back
            // edge.
            native->cpu.attach_recompiled(&code);
            native->cpu.set_verify_recompiled(true);
            native->cpu.handoff_native_call(kPollStableAddress, kIrqReturn - 4u);
            require_that(native->cpu.run_steps(2u) == 2u,
                         name + ": the loop segment and the handler's marker must be two dispatches");
            const auto& report = native->cpu.verify_report();
            // A segment that ends in a yield has no end the replay can find
            // without an instruction count: it is skipped, never verified or
            // retried, and the native result stands (the engines are compared
            // by this test's own oracle below).
            require_that(report.mismatches == 0u && report.verified == 0u && report.skipped_exit == 1u,
                         name + ": the yielding segment must be skipped, not verified or mismatched" +
                             (report.mismatch_messages.empty() ? std::string()
                                                               : ": " + report.mismatch_messages[0]));
            require_that(!native->cpu.stopped() && iterations == 1u && native->cpu.gpr[31] == 0x900u &&
                             saved_srr0(*native) == kPollStableAddress && GameTime(native->cpu) == time,
                         name + ": the segment did not end at the first iteration's back edge");
            step_to(name, *oracle, 3u);
        }
        take_interrupt(name, *oracle, false, 0x900u, kPollStableAddress, true);
        compare_irq(name, *native, *oracle);
    }
    std::cout << "PASS decrementer edge at a generated idle loop (back-edge recognition, verified)\n";
}

// branch-sys-3: mtmsr runs the instructions before it under the OLD MSR and
// recognises on both sides of the write. The decrementer's edge is delivered
// before the code is entered (see the section comment).
//   OSDisableInterrupts (EE 1 -> 0): taken BEFORE mtmsr, SRR0 = the mtmsr,
//     SRR1[EE] = 1. Writing MSR first judged the edge against EE = 0 (lost,
//     then after dispatch-exceptions-1 delayed past the critical section).
//   OSEnableInterrupts (EE 0 -> 1): latched with EE = 0, taken right after
//     mtmsr (PAGE 475), SRR0 = mtmsr + 4.
void mtmsr_recognition(RecompiledCode& code) {
    struct Case {
        IrqProgram program;
        bool ee_before;
        u64 executed;  // instructions before the recognition point: mfmsr, the rlwinm/ori, (mtmsr)
        u32 resume_offset;
    };
    const Case cases[] = {
        {GEKKO_IRQ_PROGRAM(MtmsrDisable), true, 2u, 8u},
        {GEKKO_IRQ_PROGRAM(MtmsrEnable), false, 3u, 12u},
    };
    for (const Case& test : cases) {
        const std::string name = std::string("mtmsr recognition, ") + test.program.name;
        auto native = std::make_unique<Fixture>();
        auto oracle = std::make_unique<Fixture>();
        for (Fixture* f : {native.get(), oracle.get()}) {
            load_irq_program(*f, test.program);
            if (test.ee_before) f->cpu.set_msr(f->cpu.msr() | (kMsrEE));
            f->cpu.gpr[5] = 0x55u;
            f->cpu.set_decrementer(0u);
            deliver_decrementer_edge(f->cpu);
        }
        oracle->cpu.attach_recompiled(nullptr);
        const u32 msr = native->cpu.msr();
        const GameTime time(native->cpu);
        run_generated(name, code, *native);
        const u32 resume = test.program.address + test.resume_offset;
        const Cpu& n = native->cpu;
        // mfmsr r3 read the old MSR and r4 holds the value the mtmsr writes;
        // the addi r5 after the mtmsr did not run.
        const u32 written = test.ee_before ? (msr & ~kMsrEE) : (msr | kMsrEE);
        require_that(!n.stopped() && n.exiting() && n.pc == resume && n.decrementer_pending() &&
                         n.gpr[3] == msr && n.gpr[4] == written && n.gpr[5] == 0x55u &&
                         (n.msr() & kMsrEE) != 0u && GameTime(n) == time,
                     name + ": native code did not hand back at the mtmsr recognition point (pc " +
                         std::to_string(n.pc) + ")");
        step_to(name, *oracle, test.executed);
        compare_irq(name, *native, *oracle);
        take_interrupt(name, *native, true, 0x900u, resume, true);
        take_interrupt(name, *oracle, false, 0x900u, resume, true);
        compare_irq(name, *native, *oracle);
    }
    std::cout << "PASS generated mtmsr recognition (run under the old MSR, before and after the write)\n";
}

// dispatch-exceptions-6: the external interrupt is asserted inside the call -
// by a declining hook on the callee's entry, in both engines (the hardware
// tick that asserted it during the bl went with the instruction clock). The
// callee runs and returns; the caller's post-call exiting() check must unwind
// with pc at the link address instead of running addi r4 / addi r5 / blr first.
void call_return_recognition(RecompiledCode& code) {
    const std::string name = "external interrupt asserted inside a call";
    unsigned asserted = 0;
    auto native = std::make_unique<Fixture>();
    auto oracle = std::make_unique<Fixture>();
    const IrqProgram caller = GEKKO_IRQ_PROGRAM(CallReturn);
    for (Fixture* f : {native.get(), oracle.get()}) {
        load_irq_program(*f, caller);
        f->bus.write32(kIrqCallReturnCalleeAddress, 0x38C60001u);  // addi r6,r6,1
        f->bus.write32((kIrqCallReturnCalleeAddress + 4u), 0x4E800020u);
        f->cpu.set_msr(f->cpu.msr() | (kMsrEE));
        for (u32 r = 3; r <= 6u; ++r) f->cpu.gpr[r] = r * 0x10u;
        f->cpu.install_hle(kIrqCallReturnCalleeAddress, [&asserted](Cpu& c) {
            ++asserted;
            c.set_external_interrupt(true);
            return false;
        }, "external line raised inside the call");
    }
    require_that(code.lookup(kIrqCallReturnCalleeAddress) != nullptr,
                 name + ": the callee is not a translated function of its own");
    oracle->cpu.attach_recompiled(nullptr);
    const GameTime time(native->cpu);
    run_generated(name, code, *native);
    const u32 link = caller.address + 8u;
    const Cpu& n = native->cpu;
    require_that(!n.stopped() && n.exiting() && n.pc == link && asserted == 1u &&
                     n.gpr[3] == 0x31u && n.gpr[6] == 0x61u && n.gpr[4] == 0x40u &&
                     n.gpr[5] == 0x50u && n.external_interrupt() && GameTime(n) == time,
                 name + ": the caller ran on past the return");
    step_to(name, *oracle, 4u);  // addi r3, bl, the callee's addi r6 and blr
    require_that(asserted == 2u, name + ": the interpreter must run the callee's hook once too");
    compare_irq(name, *native, *oracle);
    take_interrupt(name, *native, true, 0x500u, link, true);
    take_interrupt(name, *oracle, false, 0x500u, link, true);
    compare_irq(name, *native, *oracle);
    std::cout << "PASS generated call/return recognition (post-call exiting() unwinds at the link address)\n";
}

// branch-sys-2: a loop whose only back edge (blt in row 2) targets row 1. The
// cross-record transfer must publish pc and poll exactly like an in-body back
// edge: with a stop pending at every dispatch it yields once per iteration
// (the 1-instruction budget's job before budgets were deleted), and an
// interrupt deliverable when the loop is entered is taken at its first back
// edge, after the instructions before it ran.
void cross_record_back_edge(RecompiledCode& code) {
    constexpr u32 kIterations = 50u;
    const IrqProgram loop = GEKKO_IRQ_PROGRAM(CrossLoop);
    const u32 head = loop.address + 4u;
    require_that(code.lookup(loop.address + 8u) != nullptr && code.lookup(head) != nullptr,
                 "cross-record loop: the rows or the interior entry are missing");
    {
        const std::string name = "cross-record loop, stop pending at every dispatch";
        auto native = std::make_unique<Fixture>();
        auto oracle = std::make_unique<Fixture>();
        for (Fixture* f : {native.get(), oracle.get()}) {
            load_irq_program(*f, loop);
            f->cpu.gpr[5] = kIterations;
        }
        native->cpu.attach_recompiled(&code);
        oracle->cpu.attach_recompiled(nullptr);
        // A pause hook at both places a run can start - the entry, and the
        // loop head every yield resumes at - requests a host stop and
        // declines, so each run goes on to the next recognition point.
        u64 pauses = 0;
        const auto pause_hook = [&pauses](Cpu& c) {
            ++pauses;
            c.request_stop(StopReason::HostRequest, "cross-record pause");
            return false;
        };
        native->cpu.install_hle(loop.address, pause_hook, "cross-record entry pause");
        native->cpu.install_hle(head, pause_hook, "cross-record head pause");
        const GameTime time(native->cpu);
        const u64 interpreted = g_non_native_ppc_executions.load();
        u64 dispatches = 0;
        for (u64 runs = 0; !native->cpu.stopped() && native->cpu.pc != kIrqReturn && runs <= 4u * kIterations;
             ++runs) {
            dispatches += native->cpu.run_steps(1u);
            if (native->cpu.stop().detail == "cross-record pause") native->cpu.clear_stop();
        }
        require_that(g_non_native_ppc_executions.load() == interpreted &&
                         native->cpu.interpreted_instructions() == 0u,
                     name + ": native run used the interpreter");
        require_that(!native->cpu.stopped() && native->cpu.pc == kIrqReturn &&
                         native->cpu.gpr[3] == kIterations && GameTime(native->cpu) == time,
                     name + ": wrong loop result: " + native->cpu.stop().detail);
        require_that(dispatches == kIterations && pauses == kIterations,
                     name + ": the cross-record back edge did not poll needs_dispatch (" +
                         std::to_string(dispatches) + " dispatches)");
        // li, then per iteration addi, cmpw and blt, then blr.
        require_that(step_until(*oracle, kIrqReturn, 3u * kIterations + 3u) == 3u * kIterations + 2u &&
                         oracle->cpu.pc == kIrqReturn,
                     name + ": the interpreter did not return over the loop's instructions");
        compare_irq(name, *native, *oracle);
    }
    {
        const std::string name = "cross-record loop, interrupt deliverable at its back edge";
        auto native = std::make_unique<Fixture>();
        auto oracle = std::make_unique<Fixture>();
        for (Fixture* f : {native.get(), oracle.get()}) {
            load_irq_program(*f, loop);
            f->cpu.gpr[5] = kIterations;
            f->cpu.set_msr(f->cpu.msr() | (kMsrEE));
            f->cpu.set_external_interrupt(true);
        }
        oracle->cpu.attach_recompiled(nullptr);
        const GameTime time(native->cpu);
        run_generated(name, code, *native);
        const Cpu& n = native->cpu;
        // li, addi, cmpw ran (none is a recognition point); the taken blt is.
        require_that(!n.stopped() && n.exiting() && n.pc == head && n.gpr[3] == 1u && GameTime(n) == time,
                     name + ": the loop did not hand back at its first back edge (r3 = " +
                         std::to_string(n.gpr[3]) + ")");
        step_to(name, *oracle, 4u);
        compare_irq(name, *native, *oracle);
        take_interrupt(name, *native, true, 0x500u, head, true);
        take_interrupt(name, *oracle, false, 0x500u, head, true);
        compare_irq(name, *native, *oracle);
    }
    std::cout << "PASS generated cross-record back edge (stop-pending yield per iteration, interrupt recognition)\n";
}
} // namespace

int main() {
    RecompiledCode code;
    code.install(recomp::kFunctions, recomp::kFunctionCount);
    code.install_ranges(recomp::kRanges, recomp::kRangeCount);
    unsigned failures = 0;
    for (auto test : {traps, hooks, update_forms, reservation, count_leading_zeros,
                      overflow_forms, floating_point_forms, fp_semantics, indirect_jump_loop, shared_indirect_jumps, decrementer_idle_loop, mtmsr_recognition,
                      call_return_recognition, cross_record_back_edge}) {
        try { test(code); }
        catch (const std::exception& error) {
            std::cerr << "FAIL " << error.what() << '\n';
            ++failures;
        }
    }
    return failures == 0u ? 0 : 1;
}
