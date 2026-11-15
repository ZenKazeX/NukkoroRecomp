// Generated-code vs reference-interpreter differential suite (RC-gen-14, swarm field cs-interp).
//
// tools/ppcrecomp/make_interp_fixture.py writes ~3,000 one-row programs (every lmw/stmw rD x rA overlap, every
// lswi/stswi NB, update forms, lha/lhau, dcbz/dcbz_l offsets, stfiwx, psq forms, ps_*/fsel aliasing, mtfsf FM,
// mtfsfi, mtfsb, mcrfs, every BO of bc/bclr/bcctr, CR fusion with a label between producer and consumer, FP/HID2
// hoists around mtmsr / mtspr 920 and interior entries). The REAL ppcrecomp translates them with its production
// switches. Each program runs from identical random architected state through (a) the generated native code and
// (b) the reference interpreter, at its first instruction and - for programs that ask - at every interior
// instruction (the cold `_at` trampoline). Everything architected is compared: GPR, FPR, SPR, SR, CR, XER, LR,
// CTR, FPSCR, MSR, PC, reservation, game time and the whole data window. The native run must not touch the
// interpreter. The spec is the other engine: nothing here pins a value, so a divergence the user has ruled on
// (RC-fp-1, RC-gen-10) shows up as a failure only if the two engines stop agreeing with each other.
#include "gekko/cpu.h"
#include "gekko/cpu_diag.h"
#include "gekko/os_exception_vector.h"
#include "gekko/recomp.h"
#include "interp_fixture.h"

#include "gekko/ppc_ops.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace gekko;

namespace {
constexpr unsigned kStringFlag = 2, kVaryFp = 4, kVaryLce = 8, kSmallCtr = 16, kEntries = 1;
constexpr unsigned kDForm = 1, kXForm = 2;

constexpr u32 kContext = 0x8000A000u;
constexpr u32 kExternalHandler = 0x80009500u, kProgramHandler = 0x80009700u, kFpHandler = 0x80009800u,
              kDecrementerHandler = 0x80009900u;
constexpr u32 kReturn = 0x80009000u;
constexpr u32 kWindow = 0x80300000u, kWindowSize = 0x4000u;

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
        for (unsigned index = 0; index < kInterpProgramCount; ++index) {
            const InterpProgram& program = kInterpPrograms[index];
            for (unsigned word = 0; word < program.count; ++word)
                bus.write32(program.address + 4u * word, program.words[word]);
        }
    }
};

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

// Everything random about one case, drawn once and applied to both engines.
struct Case {
    std::array<u32, 32> gpr;
    std::array<std::array<u64, 2>, 32> fpr;
    u32 cr, xer, ctr, fpscr, msr_fp, hid2_lce;
    std::array<u32, 8> gqr;
    u32 ea;                       // the effective address the program's rA / rB are arranged to produce
    std::vector<u8> neighbourhood;  // random bytes around EA, written to both buses
    u32 neighbourhood_begin;
};

u32 draw32(std::mt19937_64& random) {
    static constexpr u32 pool[] = {0u, 1u, 2u, 3u, 4u, 7u, 8u, 31u, 32u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu,
                                   0xFFFFu, 0x10000u, 0xFFFF8000u, 0x80000001u, 0x12345678u};
    if (random() % 3u != 0u) return pool[random() % (sizeof(pool) / sizeof(pool[0]))];
    return static_cast<u32>(random());
}

u64 draw_fp(std::mt19937_64& random) {
    static constexpr u64 classes[] = {
        0x0000000000000000ull, 0x8000000000000000ull, 0x0000000000000001ull, 0x800FFFFFFFFFFFFFull,
        0x0010000000000000ull, 0x36A0000000000000ull, 0x3810000000000000ull, 0x3820000000000000ull,
        0x3FF0000000000000ull, 0xBFF8000000000000ull, 0x3FF0000002000000ull, 0x3FB999999999999Aull,
        0x47EFFFFFE0000000ull, 0x7FEFFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
        0x7FF8000000000000ull, 0xFFF8000000000000ull, 0x7FF4000000000000ull, 0xFFF0000000000001ull,
        0x41E0000000000000ull, 0x41DFFFFFFFE00000ull, 0x4000000000000000ull, 0xC000000000000000ull};
    if ((random() & 1u) == 0u) return classes[random() % (sizeof(classes) / sizeof(classes[0]))];
    u64 mantissa = random() & 0x000fffffe0000000ull;
    for (u64 bit = random() % 3u; bit != 0u; --bit) mantissa |= 1ull << (random() % 52u);
    const u64 exponent = (random() & 3u) == 0u ? random() % 2048u : 1023u - 70u + random() % 140u;
    return ((random() & 1u) << 63) | (exponent << 52) | mantissa;
}

u32 draw_fpscr(std::mt19937_64& random) {
    u32 f = 0;
    if (random() % 10u < 3u) f |= static_cast<u32>(random()) & 0x9FF80700u;  // sticky exception bits
    if (random() % 10u < 3u) f |= static_cast<u32>(random()) & 0x0001F000u;  // FPRF
    if (random() % 10u < 2u) f |= static_cast<u32>(random()) & 0x00060000u;  // FR FI
    if (random() % 4u == 0u) f |= static_cast<u32>(random()) & 0x000000F8u;  // enables
    if (random() % 100u < 45u) f |= 0x4u;                                    // NI
    if (random() % 10u < 4u) f |= 1u + static_cast<u32>(random() % 3u);      // RN
    if ((f & 0x01F80700u) != 0u) f |= 0x20000000u;                           // VX summary
    if ((((f >> 22) & f) & 0xF8u) != 0u) f |= 0x40000000u;                   // FEX summary
    return f;
}

Case draw_case(const InterpProgram& program, std::mt19937_64& random) {
    Case c{};
    for (u32& reg : c.gpr) reg = draw32(random);
    for (auto& reg : c.fpr) reg = {draw_fp(random), (random() % 10u < 7u) ? draw_fp(random) : draw_fp(random)};
    c.cr = static_cast<u32>(random());
    c.xer = static_cast<u32>(random()) & 0xE0000000u;
    if ((program.flags & kStringFlag) != 0u) {
        static constexpr u32 counts[] = {0u, 1u, 2u, 3u, 4u, 5u, 7u, 8u, 12u, 31u, 32u, 33u, 64u, 100u, 127u};
        c.xer |= counts[random() % (sizeof(counts) / sizeof(counts[0]))];
    } else {
        c.xer |= static_cast<u32>(random()) & 0x7Fu;
    }
    if ((program.flags & kSmallCtr) != 0u) {
        c.ctr = 1u + static_cast<u32>(random() % 4u);
    } else {
        static constexpr u32 ctrs[] = {0u, 1u, 2u};
        c.ctr = (random() % 3u != 0u) ? ctrs[random() % 3u] : static_cast<u32>(random());
    }
    c.fpscr = draw_fpscr(random);
    c.msr_fp = ((program.flags & kVaryFp) != 0u && random() % 5u == 0u) ? 0u : 1u;
    c.hid2_lce = ((program.flags & kVaryLce) != 0u && random() % 4u == 0u) ? 0u : 1u;
    // GQRs: every load/store type (0 and 4-7 and the reserved 1-3) with every scale, both fields.
    for (u32& gqr : c.gqr) {
        const u32 type = (random() % 4u == 0u) ? static_cast<u32>(random() % 8u) : (random() % 2u ? 0u : 4u + static_cast<u32>(random() % 4u));
        const u32 load_type = (random() % 4u == 0u) ? static_cast<u32>(random() % 8u) : (random() % 2u ? 0u : 4u + static_cast<u32>(random() % 4u));
        gqr = (static_cast<u32>(random() % 64u) << 24) | (load_type << 16) | (static_cast<u32>(random() % 64u) << 8) | type;
    }
    // The effective address: half the cases straddle a 4 KiB page boundary inside the window.
    const u32 span = program.span;
    if (program.kind != 0u) {
        const u32 lo = kWindow + 0x40u, hi = kWindow + kWindowSize - 0x40u - span;
        if (random() % 2u == 0u) {
            const u32 boundary = kWindow + 0x1000u * (1u + static_cast<u32>(random() % 3u));
            c.ea = boundary - static_cast<u32>(random() % (span + 1u));
        } else {
            c.ea = lo + static_cast<u32>(random() % (hi - lo));
        }
        if (random() % 10u < 7u) c.ea &= ~3u;
        if (program.rb == program.ra && program.ra != 0u && program.kind == kXForm) c.ea &= ~1u;
        if (c.ea < lo) c.ea = lo;
        if (c.ea > hi) c.ea = hi;
        c.neighbourhood_begin = c.ea - 0x20u;
        c.neighbourhood.resize(span + 0x40u);
        for (u8& byte : c.neighbourhood) byte = static_cast<u8>(random());
    }
    return c;
}

void apply(Fixture& f, const InterpProgram& program, const Case& c, u32 entry, std::mt19937_64& pointer_random) {
    Cpu& cpu = f.cpu;
    cpu.reset();
    for (unsigned r = 0; r < 32; ++r) cpu.gpr[r] = c.gpr[r];
    for (unsigned r = 0; r < 32; ++r) {
        cpu.fpr[r].bits[0] = c.fpr[r][0];
        cpu.fpr[r].bits[1] = c.fpr[r][1];
    }
    cpu.cr = c.cr;
    cpu.xer = c.xer;
    cpu.ctr = c.ctr;
    cpu.fpscr = c.fpscr;
    for (unsigned g = 0; g < 8; ++g) cpu.spr[SPR_GQR0 + g] = c.gqr[g];
    cpu.spr[SPR_HID2] = kHid2LSQE | kHid2PSE | (c.hid2_lce != 0u ? kHid2LCE : 0u);
    cpu.set_msr(c.msr_fp != 0u ? (cpu.msr() | kMsrFP) : (cpu.msr() & ~kMsrFP));
    cpu.lr = kReturn;
    cpu.gpr[9] = kReturn;  // the sentinel the programs reload into LR / CTR
    if (program.kind != 0u) {
        if (!c.neighbourhood.empty())
            for (u32 i = 0; i < c.neighbourhood.size(); ++i) f.bus.write8(c.neighbourhood_begin + i, c.neighbourhood[i]);
        if (program.kind == kDForm) {
            cpu.gpr[program.ra] = c.ea - static_cast<u32>(program.disp);
        } else if (program.ra == 0u) {
            cpu.gpr[program.rb] = c.ea;
        } else if (program.ra == program.rb) {
            cpu.gpr[program.ra] = c.ea / 2u;
        } else {
            const u32 base = static_cast<u32>(pointer_random());
            cpu.gpr[program.ra] = base;
            cpu.gpr[program.rb] = c.ea - base;
        }
    }
    // The three programs that write MSR / HID2 from r10 read it first (mfmsr / mfspr 920 at +4). An interior entry past that
    // read would otherwise write the case's random r10: an MSR without IR/DR or a HID2 without LSQE|PSE, states the machine
    // refuses by contract (no real mode; paired singles always enabled) - the two engines then stop at different points
    // and the case compares refusals, not semantics. Such an entry starts with r10 = what the skipped read returns.
    if (entry != program.address) {
        const std::string_view name = program.name;
        if (name == "fp_mtmsr_between") cpu.gpr[10] = cpu.msr();
        if (name == "hid2_mtspr_between" || name == "ps_after_hid2_write") cpu.gpr[10] = cpu.spr[SPR_HID2];
    }
    cpu.set_entry(entry);
}

[[noreturn]] void fail(const std::string& what, const InterpProgram& program, u32 entry_offset, unsigned iteration) {
    throw std::runtime_error(std::string(program.name) + " entry +" + std::to_string(entry_offset) + " iteration " +
                             std::to_string(iteration) + ": " + what);
}

void compare(const Fixture& n, const Fixture& o, const InterpProgram& program, u32 entry_offset, unsigned iteration) {
    const Cpu& a = n.cpu;
    const Cpu& b = o.cpu;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) fail(what, program, entry_offset, iteration);
    };
    for (unsigned r = 0; r < 32; ++r)
        if (a.gpr[r] != b.gpr[r]) {
            char text[96];
            std::snprintf(text, sizeof(text), "GPR r%u: native 0x%08X, interpreter 0x%08X", r, a.gpr[r], b.gpr[r]);
            fail(text, program, entry_offset, iteration);
        }
    for (unsigned r = 0; r < 32; ++r)
        for (unsigned half = 0; half < 2; ++half)
            if (a.fpr[r].bits[half] != b.fpr[r].bits[half]) {
                char text[128];
                std::snprintf(text, sizeof(text), "FPR f%u ps%u: native 0x%016llX, interpreter 0x%016llX", r, half,
                              static_cast<unsigned long long>(a.fpr[r].bits[half]),
                              static_cast<unsigned long long>(b.fpr[r].bits[half]));
                fail(text, program, entry_offset, iteration);
            }
    const auto word = [&](const char* name, u32 x, u32 y) {
        if (x != y) {
            char text[96];
            std::snprintf(text, sizeof(text), "%s: native 0x%08X, interpreter 0x%08X", name, x, y);
            fail(text, program, entry_offset, iteration);
        }
    };
    word("CR", a.cr, b.cr);
    word("XER", a.xer, b.xer);
    word("LR", a.lr, b.lr);
    word("CTR", a.ctr, b.ctr);
    word("FPSCR", a.fpscr, b.fpscr);
    word("MSR", a.msr(), b.msr());
    word("PC", a.pc, b.pc);
    check(a.spr == b.spr && a.decrementer() == b.decrementer() && a.sr == b.sr, "SPR / segment registers");
    check(a.reservation == b.reservation && a.reservation_address == b.reservation_address, "reservation");
    check(GameTime(a) == GameTime(b), "game time");
    check(n.bus.window_bytes(kWindow - 0x40u, kWindow + kWindowSize + 0x40u) ==
              o.bus.window_bytes(kWindow - 0x40u, kWindow + kWindowSize + 0x40u),
          "data window");
    check(n.bus.window_bytes(kContext, kContext + 0x1B0u) == o.bus.window_bytes(kContext, kContext + 0x1B0u),
          "OSContext (exception entry)");
}

unsigned g_exception_cases = 0;
// A hash multiplier for the case seeds, not a guest address.
constexpr u64 kSeedIterationMix = 0x85EBCA6Bull;  // liveness-roots: value-only

void run_case(RecompiledCode& code, Fixture& native, Fixture& oracle, const InterpProgram& program,
              u32 entry_offset, unsigned iteration, u64 seed) {
    std::mt19937_64 random(seed);
    const Case c = draw_case(program, random);
    const u64 pointer_seed = random();
    std::mt19937_64 p1(pointer_seed), p2(pointer_seed);
    const u32 entry = program.address + entry_offset;
    apply(native, program, c, entry, p1);
    apply(oracle, program, c, entry, p2);
    native.cpu.attach_recompiled(&code);
    oracle.cpu.attach_recompiled(nullptr);
    const u64 before = g_non_native_ppc_executions.load();
    if (native.cpu.run_steps(1u) != 1u) fail("the native run was not one dispatch", program, entry_offset, iteration);
    if (g_non_native_ppc_executions.load() != before || native.cpu.interpreted_instructions() != 0u)
        fail("native run used the interpreter", program, entry_offset, iteration);
    if (native.cpu.stopped()) fail("native run stopped: " + native.cpu.stop().detail, program, entry_offset, iteration);
    const bool returned = native.cpu.pc == kReturn;
    if (!returned) ++g_exception_cases;
    // The interpreter walks to the place the native run ended; a program has no loops but the bounded CTR ones.
    const u64 limit = 64u * program.count + 64u;
    u64 steps = 0;
    while (steps < limit && !oracle.cpu.stopped() && oracle.cpu.pc != native.cpu.pc) {
        oracle.cpu.step();
        ++steps;
    }
    if (oracle.cpu.stopped()) fail("interpreter stopped: " + oracle.cpu.stop().detail, program, entry_offset, iteration);
    if (oracle.cpu.pc != native.cpu.pc) fail("the interpreter did not reach the native end", program, entry_offset, iteration);
    compare(native, oracle, program, entry_offset, iteration);
}
}  // namespace

int main() {
    RecompiledCode code;
    code.install(recomp::kFunctions, recomp::kFunctionCount);
    code.install_ranges(recomp::kRanges, recomp::kRangeCount);
    unsigned iterations = 6;
    if (const char* text = std::getenv("INTERP_DIFF_ITERATIONS")) iterations = static_cast<unsigned>(std::strtoul(text, nullptr, 10));
    auto native = std::make_unique<Fixture>();
    auto oracle = std::make_unique<Fixture>();
    unsigned cases = 0, failures = 0, entries = 0;
    for (unsigned index = 0; index < kInterpProgramCount; ++index) {
        const InterpProgram& program = kInterpPrograms[index];
        if (code.lookup(program.address) == nullptr) {
            std::cerr << "FAIL " << program.name << ": the generator did not translate the program\n";
            ++failures;
            continue;
        }
        const unsigned offsets = (program.flags & kEntries) != 0u ? program.count : 1u;
        for (unsigned instruction = 0; instruction < offsets; ++instruction) {
            ++entries;
            for (unsigned iteration = 0; iteration < iterations; ++iteration) {
                const u64 seed = 0x9E3779B97F4A7C15ull * (index + 1u) + 0x1000193ull * instruction + kSeedIterationMix * iteration;
                try {
                    run_case(code, *native, *oracle, program, 4u * instruction, iteration, seed);
                    ++cases;
                } catch (const std::exception& error) {
                    std::cerr << "FAIL " << error.what() << '\n';
                    if (++failures >= 25u) return 1;
                    // A failed case leaves the engines apart: start the next one from fresh fixtures.
                    native = std::make_unique<Fixture>();
                    oracle = std::make_unique<Fixture>();
                }
            }
        }
    }
    std::cout << (failures == 0u ? "PASS " : "DONE ") << cases << " generated-vs-interpreter cases over "
              << kInterpProgramCount << " programs (" << entries << " entries, " << g_exception_cases
              << " ended in an exception vector)\n";
    return failures == 0u ? 0 : 1;
}
