// liveness-roots: reference-interpreter-only
// Original-DOL contract for the owned PPCHalt (gekko/native_os_panic.h): the original executes `sync` and an endless
// loop - for every caller state (link register, r3) it never leaves 0x80370B1C..0x80370B2F, never returns to the link
// register and writes no memory; the owner stops the run with an ExecutionFault naming the link register
// (EXPLICIT DIFFERENCE: stop instead of spinning). usage: os_halt_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_os_panic.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
void require(bool ok, const std::string& m) { if (!ok) throw std::runtime_error(m); }
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: os_halt_contract_tests main.dol");
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        GuestMemory memory;
        Cpu cpu{memory};
        const auto word = [&](std::size_t o) {
            return u32{dol[o]} << 24 | u32{dol[o + 1]} << 16 | u32{dol[o + 2]} << 8 | u32{dol[o + 3]};
        };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (size != 0u) memory.write_block(address, dol.data() + offset, size);
        }
        unsigned runs = 0;
        for (u32 lr : {0x80100000u, 0x803A3BE0u, 0u, 0xFFFFFFFCu})
            for (u32 r3 : {0u, 1u, 0xDEADBEEFu}) {
                cpu.reset();
                cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
                cpu.gpr[1] = 0x817E0000u;
                cpu.gpr[3] = r3;
                cpu.lr = lr;
                cpu.pc = os_panic::kPpcHalt;
                cpu.npc = os_panic::kPpcHalt + 4u;
                for (unsigned step = 0; step < 5000; ++step) {
                    cpu.run_steps(1u);
                    require(!cpu.stopped() && cpu.pc >= os_panic::kPpcHalt && cpu.pc < os_panic::kPpcHalt + 0x14u,
                            "the original left the PPCHalt loop");
                }
                cpu.reset();
                cpu.lr = lr;
                os_panic::serve_halt(cpu);
                char want[32];
                std::snprintf(want, sizeof(want), "0x%08X", lr);
                require(cpu.stopped() && cpu.stop().reason == StopReason::ExecutionFault &&
                            cpu.stop().detail.find(want) != std::string::npos,
                        "the owner must stop with a fault naming the link register");
                ++runs;
            }
        std::printf("PASS os halt: %u variations; the original never leaves its 5-instruction loop in 5000 steps, the owner "
                    "stops with an ExecutionFault naming LR (stated difference: stop instead of spinning)\n", runs);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL os halt contract: %s\n", e.what());
        return 1;
    }
}
