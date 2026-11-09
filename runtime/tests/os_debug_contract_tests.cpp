// Original-DOL ownership oracle for InitializeUART (gekko/native_os_debug.h).
// liveness-roots: reference-interpreter-only
// No generated reference body: every SDK instruction comes from the pinned DOL. The owner and the original run on
// identical machines (registers, floating-point registers, memory) and the whole architected state afterwards is
// compared, apart from the original's dead frame words (back chain, saved link register), r0 and cr0.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_os_debug.h"

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
using namespace gekko::os_debug;

constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kWatchLow = 0x805E0000u, kWatchBytes = 0x30000u;  // the small-data area around r13
constexpr u32 kScratch = 0x81000000u, kScratchBytes = 0x10000u;
constexpr u32 kStackWindow = 0x817DFF00u, kStackBytes = 0x200u;

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
        memory.write_block(kStackWindow, zero.data(), kStackBytes);
    }

    // The same starting state for every run: registers get a pattern, so a register an owner changes shows up.
    void prepare() {
        cpu.reset();
        cpu.fpscr = 0;
        for (unsigned i = 0; i < 32; ++i) {
            cpu.gpr[i] = 0x10101010u * (i + 1u) + 7u;
            cpu.fpr[i] = FpReg{};
        }
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13;
        cpu.cr = 0x12345678u; cpu.xer = 0xA0000000u; cpu.ctr = 0x55AA55AAu;
        cpu.lr = kReturn;
        memory.write_block(kStackWindow, std::vector<u8>(kStackBytes).data(), kStackBytes);
    }
    void original() {
        cpu.pc = kInitializeUart; cpu.npc = kInitializeUart + 4;
        const u64 tick = cpu.game_clock().now();
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 1000) { cpu.run_steps(1); ++instructions; }
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack, "original call failed return contract");
        require(cpu.game_clock().now() == tick, "the original moved the game clock");
    }
    struct State {
        u32 gpr[32], cr, xer, ctr, lr;
        std::vector<u8> fpr, small_data, scratch, stack;
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
        s.stack.resize(kStackBytes);
        memory.read_block(kStackWindow, s.stack.data(), s.stack.size());
        return s;
    }
};

u32 word_at(const std::vector<u8>& bytes, u32 offset) { return load_be32(bytes.data() + offset); }

struct Input {
    u32 brought_up;    // the word at r13 + 0x5EE4
    u32 bi2;           // the pointer at r13 + 0x5A20 (0, or kScratch holding the block)
    u32 console_type;  // the block's consoleType
    u32 cell_fill;     // the previous content of 0x5EE0 / 0x5ED8 / 0x5EDC
};

void seed(GuestMemory& memory, const Input& in) {
    memory.write32(kR13 + kUartBroughtUp, in.brought_up);
    memory.write32(kR13 + kBi2Cell, in.bi2);
    if (in.bi2 != 0u) memory.write32(in.bi2 + kConsoleTypeOffset, in.console_type);
    memory.write32(kR13 + kUartEnabledCell, in.cell_fill);
    memory.write32(kR13 + kUartCell0, in.cell_fill);
    memory.write32(kR13 + kUartCell1, in.cell_fill);
}

struct Totals { unsigned cases = 0, returned_0 = 0, returned_2 = 0; };

// Original and owner on the same input; everything the architecture holds is equal afterwards except r0, cr0 (scratch of
// the original) and the two frame words it writes; those are checked to hold exactly what the prologue stores.
bool compare(const Input& in, const std::function<void(Cpu&)>& owner, Machine& original, Machine& native,
             Totals* totals) {
    original.prepare(); native.prepare();
    seed(original.memory, in); seed(native.memory, in);
    original.original();
    owner(native.cpu);
    const Machine::State a = original.state(), b = native.state();
    bool equal = true;
    for (unsigned i = 0; i < 32; ++i) {
        if (i == 0) {
            if (totals) require(a.gpr[0] == kReturn, "the original's r0 is not the reloaded link register");
            continue;
        }
        equal = equal && a.gpr[i] == b.gpr[i];
    }
    equal = equal && (a.cr & 0x0FFFFFFFu) == (b.cr & 0x0FFFFFFFu) && a.xer == b.xer && a.ctr == b.ctr && a.lr == b.lr;
    equal = equal && a.fpr == b.fpr && a.small_data == b.small_data && a.scratch == b.scratch;
    // The stack window: the original wrote the back chain at -8(sp) and the link register at 4(sp); nothing else.
    const u32 back_chain = kStack - 8u - kStackWindow, saved_lr = kStack + 4u - kStackWindow;
    for (u32 offset = 0; offset < kStackBytes; offset += 4) {
        if (offset == back_chain || offset == saved_lr) continue;
        equal = equal && word_at(a.stack, offset) == word_at(b.stack, offset);
    }
    if (totals) {
        require(word_at(a.stack, back_chain) == kStack && word_at(a.stack, saved_lr) == kReturn,
                "the original's frame words are not the documented back chain / link register");
        require(word_at(b.stack, back_chain) == 0u && word_at(b.stack, saved_lr) == 0u,
                "the owner wrote a frame word");
        ++totals->cases;
        (a.gpr[3] == 0u ? totals->returned_0 : totals->returned_2) += 1u;
    }
    return equal;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: os_debug_contract_tests main.dol\n"); return 2; }
    try {
        Machine original(argv[1]), native(argv[1]);
        Totals totals;
        const u32 brought_up_values[] = {kUartEnabled, 0u, 1u, 0xFFFFFFFFu, kUartEnabled ^ 1u, kUartEnabled ^ 0x80000000u};
        const u32 types[] = {0u, 1u, 3u, 0x10000000u, 0x10000002u, 0x10000006u, 0x20000000u, 0x30000001u, 0x0FFFFFFFu,
                             0xEFFFFFFFu, 0xFFFFFFFFu, 0x08000000u};
        const u32 fills[] = {0xDEADBEEFu, kUartEnabled, 0u};
        std::vector<Input> inputs;
        for (u32 up : brought_up_values)
            for (u32 fill : fills) {
                inputs.push_back({up, 0u, 0u, fill});  // no block: the default type
                for (u32 type : types) inputs.push_back({up, kScratch, type, fill});
            }
        for (const Input& in : inputs)
            require(compare(in, serve_initialize_uart, original, native, &totals), "InitializeUART differs");
        require(totals.returned_0 != 0u && totals.returned_2 != 0u, "the sweep did not reach both results");

        // Negative controls: an owner with one mistake each must be caught by the same comparison.
        const auto caught = [&](const char* name, const std::function<void(Cpu&)>& mutant) {
            for (const Input& in : inputs)
                if (!compare(in, mutant, original, native, nullptr)) return;
            throw std::runtime_error(std::string("negative control not detected: ") + name);
        };
        caught("retail result 0", [](Cpu& c) { serve_initialize_uart(c); if (c.gpr[3] == 2u) c.gpr[3] = 0u; });
        caught("wrong console type bit", [](Cpu& c) {
            const u32 base = c.gpr[13], bi2 = c.mem_read32(base + kBi2Cell);
            u32 type = bi2 != 0u ? c.mem_read32(bi2 + kConsoleTypeOffset) : 0u;
            if (type == 0u) type = kDefaultConsoleType;
            if (c.mem_read32(base + kUartBroughtUp) == kUartEnabled) { c.gpr[3] = 0u; return; }
            if ((type & 0x20000000u) == 0u) { c.mem_write32(base + kUartEnabledCell, 0u); c.gpr[3] = 2u; return; }
            c.mem_write32(base + kUartEnabledCell, kUartEnabled);
            c.mem_write32(base + kUartCell0, 0u); c.mem_write32(base + kUartCell1, 1u); c.gpr[3] = 0u;
        });
        caught("cell 0x5ED8 not cleared", [](Cpu& c) {
            const u32 saved = c.mem_read32(c.gpr[13] + kUartCell0);
            serve_initialize_uart(c);
            c.mem_write32(c.gpr[13] + kUartCell0, saved);
        });
        caught("writes on the already-up path", [](Cpu& c) {
            if (c.mem_read32(c.gpr[13] + kUartBroughtUp) == kUartEnabled)
                c.mem_write32(c.gpr[13] + kUartEnabledCell, kUartEnabled);
            serve_initialize_uart(c);
        });
        caught("zero type is not the default", [](Cpu& c) {
            const u32 bi2 = c.mem_read32(c.gpr[13] + kBi2Cell);
            if (bi2 != 0u && c.mem_read32(bi2 + kConsoleTypeOffset) == 0u &&
                c.mem_read32(c.gpr[13] + kUartBroughtUp) != kUartEnabled) {
                c.mem_write32(c.gpr[13] + kUartEnabledCell, 0u);
                c.gpr[3] = 2u;
                return;
            }
            serve_initialize_uart(c);
        });

        {
            GuestMemory memory;
            Cpu cpu(memory);
            install(cpu);
            require(cpu.has_hle(kInitializeUart), "the owner is not installed at its address");
        }

        std::printf("PASS os debug contract: %u inputs against the original (result 0 in %u, 2 in %u), %llu original "
                    "instructions, the original's two dead frame words checked in each, 5 negative controls detected\n",
                    totals.cases, totals.returned_0, totals.returned_2,
                    static_cast<unsigned long long>(original.instructions));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL os debug contract: %s\n", error.what());
        return 1;
    }
}
