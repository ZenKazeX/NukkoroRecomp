// Original-DOL ownership oracle for MetroTRK's remaining entries (gekko/native_trk.h).
// liveness-roots: reference-interpreter-only
// DBInit and the three console procedures run from the pinned DOL in the reference interpreter next to the owners on
// identical machines. Compared: r3 (where defined), r1 / r2 / r13..r31, lr, the FPRs, the low-memory block, the
// small-data area, both TRK cells and a scratch area. Documented differences: r0, r3, r4 (DBInit's scratch), ctr, cr0,
// and the stack frames the console procedures push (their words are excluded and the owner is asserted to write none).
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_trk.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
using namespace gekko::trk;

constexpr u32 kReturn = 0x817FF000u, kStack = 0x81170000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kLowBytes = 0x100u;
constexpr u32 kSdaLow = 0x805E0000u, kSdaBytes = 0x30000u;
constexpr u32 kCellsLow = 0x805A4000u, kCellsBytes = 0x2000u;  // both TRK cells
constexpr u32 kScratch = 0x81000000u, kScratchBytes = 0x10000u;
constexpr u32 kStackWindow = 0x8116FF00u, kStackBytes = 0x200u;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    u64 instructions = 0;

    explicit Machine(const char* path) {
        std::ifstream input(path, std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        // SHA-256: fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af.
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
    }
    void prepare() {
        cpu.reset();
        cpu.fpscr = 0;
        for (unsigned i = 0; i < 32; ++i) {
            cpu.gpr[i] = 0x10101010u * (i + 1u) + 7u;
            cpu.fpr[i] = FpReg{};
        }
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13;
        cpu.cr = 0x12345678u; cpu.xer = 0xA0000000u; cpu.ctr = 0x55AA55AAu; cpu.lr = kReturn;
    }
    void original(u32 entry) {
        cpu.pc = entry; cpu.npc = entry + 4;
        const u64 tick = cpu.game_clock().now();
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 100000) { cpu.run_steps(1); ++instructions; }
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack, "original call failed return contract");
        require(cpu.game_clock().now() == tick, "the original moved the game clock");
    }
    struct State {
        u32 gpr[32], cr, xer, lr;
        std::vector<u8> fpr, low, sda, cells, scratch, stack;
    };
    State state() {
        State s{};
        std::memcpy(s.gpr, cpu.gpr, sizeof(s.gpr));
        s.cr = cpu.cr; s.xer = cpu.xer; s.lr = cpu.lr;
        s.fpr.resize(sizeof(cpu.fpr)); std::memcpy(s.fpr.data(), cpu.fpr, sizeof(cpu.fpr));
        s.low.resize(kLowBytes); memory.read_block(0x80000000u, s.low.data(), s.low.size());
        s.sda.resize(kSdaBytes); memory.read_block(kSdaLow, s.sda.data(), s.sda.size());
        s.cells.resize(kCellsBytes); memory.read_block(kCellsLow, s.cells.data(), s.cells.size());
        s.scratch.resize(kScratchBytes); memory.read_block(kScratch, s.scratch.data(), s.scratch.size());
        s.stack.resize(kStackBytes); memory.read_block(kStackWindow, s.stack.data(), s.stack.size());
        return s;
    }
};

// `scratch_registers`: registers the original is allowed to change (compared otherwise); `stack_free`: ignore the stack
// window (the original's frame), and require the owner to have left it alone.
bool equal_states(const Machine::State& a, const Machine::State& b, bool compare_r3, bool dbinit, bool stack_free,
                  const Machine::State& zero_stack) {
    bool equal = !compare_r3 || a.gpr[3] == b.gpr[3];
    for (unsigned i : {1u, 2u, 13u, 14u, 15u, 16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 31u})
        equal = equal && a.gpr[i] == b.gpr[i];
    if (!dbinit) equal = equal && a.gpr[4] == b.gpr[4];  // the console procedures do not touch r4 only when returning early
    equal = equal && (a.cr & 0x0FFFFFFFu) == (b.cr & 0x0FFFFFFFu) && a.xer == b.xer && a.lr == b.lr && a.fpr == b.fpr &&
            a.low == b.low && a.sda == b.sda && a.cells == b.cells && a.scratch == b.scratch;
    if (stack_free) equal = equal && b.stack == zero_stack.stack;
    else equal = equal && a.stack == b.stack;
    return equal;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: trk_contract_tests main.dol\n"); return 2; }
    try {
        Machine original(argv[1]), native(argv[1]);
        unsigned cases = 0, stops = 0;
        const auto setup = [&](Machine& m, u32 flag, u32 nub, u32 low_fill) {
            const std::vector<u8> zero(kScratchBytes);
            m.memory.write_block(kScratch, zero.data(), zero.size());
            m.memory.write_block(kStackWindow, std::vector<u8>(kStackBytes).data(), kStackBytes);
            for (u32 off = 0; off < kLowBytes; off += 4) m.memory.write32(0x80000000u + off, low_fill + off);
            m.memory.write8(kConsoleEnabled, static_cast<u8>(flag));
            m.memory.write32(kNubState, nub);
            m.memory.write32(kR13 + kSdaDbInterface, 0xDEADBEEFu);
            m.memory.write32(kR13 + kSdaDbVerbosity, 0xCAFEBABEu);
            m.prepare();
        };
        const auto compare = [&](u32 entry, void (*owner)(Cpu&), u32 flag, u32 nub, u32 low_fill, bool compare_r3, bool dbinit,
                                 bool stack_free) {
            setup(original, flag, nub, low_fill);
            setup(native, flag, nub, low_fill);
            const Machine::State zero_stack = native.state();
            original.original(entry);
            owner(native.cpu);
            require(!native.cpu.stopped(), "the owner stopped the run");
            return equal_states(original.state(), native.state(), compare_r3, dbinit, stack_free, zero_stack);
        };

        // DBInit: no inputs; the low-memory words around it hold different content each time.
        for (u32 fill : {0u, 0x11110000u, 0xFFFFFF00u, 0x80000000u}) {
            require(compare(kDbInit, serve_db_init, 0, 0, fill, false, true, false), "DBInit differs");
            ++cases;
        }

        // The three console procedures, with the debugger absent (the PC state) in every flag combination.
        struct Proc { const char* name; u32 address; void (*serve)(Cpu&); };
        const Proc procs[] = {{"close", kConsoleClose, serve_console_close},
                              {"write", kConsoleWrite, serve_console_write},
                              {"read", kConsoleRead, serve_console_read}};
        for (const Proc& p : procs)
            for (u32 flag : {0u, 1u, 0x80u, 0xFFu}) {
                // nub down: the original returns 1 without a message
                require(compare(p.address, p.serve, flag, 0u, 0x22220000u, true, false, true),
                        (std::string("TRK console ") + p.name + " differs").c_str());
                ++cases;
            }
        // The nub up (never in this program): the original would build a TRK message; the owner stops the run.
        // For write and read the flag byte also gates it.
        for (const Proc& p : procs)
            for (u32 nub : {1u, 0x80500000u, 0xFFFFFFFFu}) {
                setup(native, 1u, nub, 0u);
                p.serve(native.cpu);
                require(native.cpu.stopped(), "an owner did not stop with the nub up");
                ++stops;
            }
        // write and read with the flag byte clear return before looking at the nub: equal to the original even with it up.
        for (const Proc& p : {procs[1], procs[2]}) {
            require(compare(p.address, p.serve, 0u, 0x80500000u, 0x33330000u, true, false, true), "flag-clear path differs");
            ++cases;
        }

        // Negative controls.
        const auto must_differ = [&](bool equal, const char* what) { require(!equal, what); };
        must_differ(compare(kDbInit, [](Cpu& c) { serve_db_init(c); c.mem_write32(c.gpr[13] + kSdaDbVerbosity, 0u); }, 0, 0, 0u, false, true, false),
                    "negative control (verbosity flag) was not detected");
        must_differ(compare(kDbInit, [](Cpu& c) { c.mem_write32(c.gpr[13] + kSdaDbInterface, kDbInterface);
                                                  c.mem_write32(c.gpr[13] + kSdaDbVerbosity, 1u); }, 0, 0, 0u, false, true, false),
                    "negative control (no hook word) was not detected");
        must_differ(compare(kConsoleClose, [](Cpu& c) { c.gpr[3] = 0u; }, 0, 0, 0u, true, false, true),
                    "negative control (close returns 0) was not detected");
        must_differ(compare(kConsoleWrite, [](Cpu& c) { c.gpr[3] = 1u; c.mem_write8(kConsoleEnabled, 1u); }, 0, 0, 0u, true, false, true),
                    "negative control (write sets the flag) was not detected");

        {
            GuestMemory memory;
            Cpu cpu(memory);
            install(cpu);
            std::size_t count = 0;
            const Owners::Address* owners = Owners::addresses(count);
            require(count == 4u, "unexpected owner count");
            for (std::size_t i = 0; i < count; ++i) require(cpu.has_hle(owners[i].address), "an owner is not installed");
        }
        std::printf("PASS trk contract: %u cases against the original, %u nub-up cases stop the owner, %llu original "
                    "instructions, 4 negative controls detected\n", cases, stops, static_cast<unsigned long long>(original.instructions));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL trk contract: %s\n", error.what());
        return 1;
    }
}
