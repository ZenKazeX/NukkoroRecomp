// Original-DOL ownership oracle for the OS arena accessors and the time-base readers (gekko/native_os_arena_clock.h).
// liveness-roots: reference-interpreter-only
// No generated reference body: every SDK instruction comes from the pinned DOL. The owner and the original run on
// identical machines (registers, floating-point registers, memory) and the whole architected state afterwards is
// compared, apart from the two documented differences of OSGetTime (r5 and cr0 field 0).
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_os_arena_clock.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
using namespace gekko;
using namespace gekko::os_arena_clock;

constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kWatchLow = 0x805E0000u, kWatchBytes = 0x130000u;  // the small-data areas around every r13 used
constexpr u32 kScratch = 0x81000000u, kScratchBytes = 0x10000u;

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
        const std::vector<u8> zero(kScratchBytes);
        memory.write_block(kScratch, zero.data(), zero.size());
        memory.write_block(kReturn - 0x100u, zero.data(), 0x200u);
    }

    // The same starting state for every run: registers get a pattern, so a register an owner changes shows up.
    void prepare(u32 r13, u32 r3, u32 r4, u64 time_base) {
        cpu.reset();
        cpu.fpscr = 0;
        for (unsigned i = 0; i < 32; ++i) {
            cpu.gpr[i] = 0x10101010u * (i + 1u) + 7u;
            cpu.fpr[i] = FpReg{};
        }
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = r13; cpu.gpr[3] = r3; cpu.gpr[4] = r4;
        cpu.cr = 0x12345678u; cpu.xer = 0xA0000000u; cpu.ctr = 0x55AA55AAu;
        cpu.lr = kReturn;
        cpu.set_time_base(time_base);
    }
    void original(u32 entry) {
        cpu.pc = entry; cpu.npc = entry + 4;
        const u64 tick = cpu.game_clock().now();
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 1000) { cpu.run_steps(1); ++instructions; }
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack, "original call failed return contract");
        require(cpu.game_clock().now() == tick, "the original moved the game clock");
    }
    struct State {
        u32 gpr[32], cr, xer, ctr, lr;
        std::vector<u8> fpr, small_data, scratch;
        u64 time_base;
    };
    State state() {
        State s{};
        std::memcpy(s.gpr, cpu.gpr, sizeof(s.gpr));
        s.cr = cpu.cr; s.xer = cpu.xer; s.ctr = cpu.ctr; s.lr = cpu.lr;
        s.fpr.resize(sizeof(cpu.fpr));
        std::memcpy(s.fpr.data(), cpu.fpr, sizeof(cpu.fpr));
        s.small_data.resize(kWatchBytes);
        memory.read_block(kWatchLow, s.small_data.data(), s.small_data.size());
        s.scratch.resize(kScratchBytes);
        memory.read_block(kScratch, s.scratch.data(), s.scratch.size());
        s.time_base = cpu.time_base();
        return s;
    }
};

struct Diff {
    bool registers = false, memory = false;
    bool r5 = false, cr0 = false;
};

Diff compare(const Machine::State& a, const Machine::State& b) {
    Diff d;
    for (unsigned i = 0; i < 32; ++i) {
        if (a.gpr[i] == b.gpr[i]) continue;
        if (i == 5) d.r5 = true; else d.registers = true;
    }
    if ((a.cr & 0x0FFFFFFFu) != (b.cr & 0x0FFFFFFFu)) d.registers = true;
    if ((a.cr & 0xF0000000u) != (b.cr & 0xF0000000u)) d.cr0 = true;
    if (a.xer != b.xer || a.ctr != b.ctr || a.lr != b.lr || a.time_base != b.time_base) d.registers = true;
    if (a.fpr != b.fpr) d.registers = true;
    if (a.small_data != b.small_data || a.scratch != b.scratch) d.memory = true;
    return d;
}

using Serve = std::function<void(Cpu&)>;

// One input on two identical machines: the original DOL body on `original`, the owner on `native`. Returns the
// differences; `inputs` is applied by `seed` (cell contents) before each run.
struct Case {
    u32 entry;
    Serve owner;
    u32 r13, r3, r4;
    u64 time_base;
    std::function<void(GuestMemory&, u32 r13)> seed;
};

Diff run_case(Machine& original, Machine& native, const Case& c) {
    original.prepare(c.r13, c.r3, c.r4, c.time_base);
    native.prepare(c.r13, c.r3, c.r4, c.time_base);
    if (c.seed) { c.seed(original.memory, c.r13); c.seed(native.memory, c.r13); }
    original.original(c.entry);
    c.owner(native.cpu);
    return compare(original.state(), native.state());
}

struct Totals { unsigned cases = 0, r5_only = 0, cr0_only = 0; };

void expect_equal(const Diff& d, const char* what, bool allow_time_scratch, Totals& totals) {
    ++totals.cases;
    require(!d.registers, what);
    require(!d.memory, what);
    if (!allow_time_scratch) require(!d.r5 && !d.cr0, what);
    if (d.r5) ++totals.r5_only;
    if (d.cr0) ++totals.cr0_only;
}

const u32 kValues[] = {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0x80A00000u, 0x81700000u, 0x817FFFE0u, 0xFFFFFFFFu,
                       0x12345678u, 0xDEADBEEFu};
const u32 kBases[] = {kR13, 0x805EC504u, 0x80600000u, 0x80700010u};
const u64 kTimes[] = {0ull, 1ull, 0xFFFFFFFFull, 0x100000000ull, 0x1FFFFFFFFull, 0x7FFFFFFFFFFFFFFFull,
                      0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 0x0000123456789ABCull, 0xFEDCBA9876543210ull,
                      40500000ull * 3600ull, 40500000ull * 86400ull * 400ull};

// A cell holds `value` at base + offset; every other word of the small-data area carries a sentinel, so a store to a
// wrong cell or a read of a wrong cell is visible.
std::function<void(GuestMemory&, u32)> cells(u32 hi, u32 lo) {
    return [hi, lo](GuestMemory& memory, u32 r13) {
        for (u32 offset = 0; offset < 0x8000u; offset += 4) memory.write32(r13 - 0x4000u + offset, 0x5A5A0000u + offset);
        memory.write32(r13 + kArenaHiCell, hi);
        memory.write32(r13 + kArenaLoCell, lo);
    };
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: os_arena_clock_contract_tests main.dol\n"); return 2; }
    try {
        Machine original(argv[1]), native(argv[1]);
        Totals totals;

        // The arena cells: every value at every base, with a different value in the other cell.
        for (u32 base : kBases)
            for (u32 value : kValues) {
                const u32 other = value ^ 0xA5A5A5A5u;
                expect_equal(run_case(original, native, {kGetArenaHi, serve_get_arena_hi, base, 0x11111111u, 0u, 0,
                                                         cells(value, other)}), "OSGetArenaHi differs", false, totals);
                expect_equal(run_case(original, native, {kGetArenaLo, serve_get_arena_lo, base, 0x11111111u, 0u, 0,
                                                         cells(other, value)}), "OSGetArenaLo differs", false, totals);
                expect_equal(run_case(original, native, {kSetArenaLo, serve_set_arena_lo, base, value, 0u, 0,
                                                         cells(other, other)}), "OSSetArenaLo differs", false, totals);
            }

        // The clock: every time base, every register pattern in r3 / r4 (both are overwritten).
        for (u64 time : kTimes) {
            const Case get_time{kGetTime, serve_get_time, kR13, 0x22222222u, 0x33333333u, time, nullptr};
            const Case get_tick{kGetTick, serve_get_tick, kR13, 0x22222222u, 0x33333333u, time, nullptr};
            const Diff d = run_case(original, native, get_time);
            expect_equal(d, "OSGetTime differs", true, totals);
            require(d.r5 && d.cr0, "the documented r5 / cr0 difference of OSGetTime was not seen");
            expect_equal(run_case(original, native, get_tick), "OSGetTick differs", false, totals);
        }

        // Negative controls: an owner with one mistake each must be caught by the same comparison.
        const auto must_differ = [&](const Case& c, const char* what) {
            const Diff d = run_case(original, native, c);
            require(d.registers || d.memory, what);
        };
        must_differ({kGetArenaHi, [](Cpu& c) { c.gpr[3] = c.mem_read32(c.gpr[13] + kArenaLoCell); }, kR13, 0, 0, 0,
                     cells(0x81700000u, 0x80A00000u)}, "negative control (hi reads the lo cell) was not detected");
        must_differ({kSetArenaLo, [](Cpu& c) { c.mem_write32(c.gpr[13] + kArenaHiCell, c.gpr[3]); }, kR13,
                     0x80A00000u, 0, 0, cells(1, 2)}, "negative control (lo store hits the hi cell) was not detected");
        must_differ({kGetArenaLo, [](Cpu& c) { c.gpr[3] = c.mem_read32(0x805EC500u + kArenaLoCell); }, 0x80600000u, 0,
                     0, 0, cells(1, 2)}, "negative control (fixed r13) was not detected");
        must_differ({kGetTime, [](Cpu& c) { serve_get_time(c); c.gpr[4] += 1u; }, kR13, 0, 0, 5ull,
                     nullptr}, "negative control (low word off by one) was not detected");
        must_differ({kGetTick, [](Cpu& c) { c.gpr[3] = static_cast<u32>(c.time_base() >> 32); }, kR13, 0, 0,
                     0x100000007ull, nullptr}, "negative control (tick reads the high word) was not detected");

        // The owners are registered on a CPU, and for exactly the addresses the owner list names.
        {
            GuestMemory memory;
            Cpu cpu(memory);
            install(cpu);
            std::size_t count = 0;
            const Owners::Address* owners = Owners::addresses(count);
            require(count == 5u, "unexpected owner count");
            for (std::size_t i = 0; i < count; ++i)
                require(cpu.has_hle(owners[i].address), "an owner is not installed at its address");
        }

        std::printf("PASS os arena + clock contract: %u cases against the original (r5 differs in %u, cr0 in %u: the "
                    "documented OSGetTime differences), %llu original instructions, 5 negative controls detected\n",
                    totals.cases, totals.r5_only, totals.cr0_only, static_cast<unsigned long long>(original.instructions));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL os arena + clock contract: %s\n", error.what());
        return 1;
    }
}
