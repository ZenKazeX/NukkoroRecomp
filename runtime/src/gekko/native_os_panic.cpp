// The native owner of OSPanic (gekko/native_os_panic.h).

#include "gekko/native_os_panic.h"

#include "gekko/cpu.h"

#include <cstdio>

namespace gekko::os_panic {

namespace {

std::string read_text(Cpu& cpu, u32 address) {
    std::string text;
    if (address == 0u) return "(null)";
    for (u32 i = 0; i < 160u; ++i) {
        const u8 c = cpu.mem_read8(address + i);
        if (c == 0u) return text;
        text.push_back(c >= 0x20u && c < 0x7Fu ? static_cast<char>(c) : (c == '\n' ? ' ' : '?'));
    }
    return text + "...";
}

}  // namespace

std::string describe(Cpu& cpu) {
    char line[24];
    std::snprintf(line, sizeof(line), "%d", static_cast<int>(cpu.gpr[4]));
    return read_text(cpu, cpu.gpr[3]) + ":" + line + ": " + read_text(cpu, cpu.gpr[5]);
}

bool serve_panic(Cpu& cpu) {
    const bool game_end = cpu.lr == kGameEndReturn;
    char from[32];
    std::snprintf(from, sizeof(from), "0x%08X", cpu.lr);
    if (game_end) {
        cpu.request_stop(StopReason::HleExit,
                         "the game ended itself with OSPanic (" + describe(cpu) + "; from LR " + from +
                             "): the console halts there, the program ends");
    } else {
        cpu.request_stop(StopReason::ExecutionFault,
                         "OSPanic (" + describe(cpu) + "; from LR " + from + "): the console halts there");
    }
    return game_end;
}

void serve_halt(Cpu& cpu) {
    char detail[96];
    std::snprintf(detail, sizeof(detail), "PPCHalt (from LR 0x%08X): the console halts there (an endless loop)", cpu.lr);
    cpu.request_stop(StopReason::ExecutionFault, detail);
}

void install(Cpu& cpu) {
    cpu.install_hle(kOSPanic, [](Cpu& c) {
        serve_panic(c);
        return true;
    }, "OSPanic (native)");
    cpu.install_hle(kPpcHalt, [](Cpu& c) {
        serve_halt(c);
        return true;
    }, "PPCHalt (native)");
    cpu.install_hle(kHaltWrapper, [](Cpu& c) {
        serve_halt(c);  // its only effect is the call of PPCHalt
        return true;
    }, "the halt wrapper (native)");
}

}  // namespace gekko::os_panic
