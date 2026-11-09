// The native owner of InitializeUART (gekko/native_os_debug.h).

#include "gekko/native_os_debug.h"

#include "gekko/cpu.h"

namespace gekko::os_debug {

u32 console_type(Cpu& cpu) {
    const u32 bi2 = cpu.mem_read32(cpu.gpr[13] + kBi2Cell);
    const u32 type = bi2 != 0u ? cpu.mem_read32(bi2 + kConsoleTypeOffset) : 0u;
    return type == 0u ? kDefaultConsoleType : type;
}

void serve_initialize_uart(Cpu& cpu) {
    const u32 base = cpu.gpr[13];
    if (cpu.mem_read32(base + kUartBroughtUp) == kUartEnabled) {
        cpu.gpr[3] = 0u;
        return;
    }
    const u32 type = console_type(cpu);
    if ((type & kDevelopmentBit) == 0u) {
        cpu.mem_write32(base + kUartEnabledCell, 0u);
        cpu.gpr[3] = 2u;
        return;
    }
    cpu.mem_write32(base + kUartEnabledCell, kUartEnabled);
    cpu.mem_write32(base + kUartCell0, 0u);
    cpu.mem_write32(base + kUartCell1, 1u);
    cpu.gpr[3] = 0u;
}

void install(Cpu& cpu) {
    cpu.install_hle(kInitializeUart, [](Cpu& c) {
        serve_initialize_uart(c);
        return true;  // an owner never declines
    }, "InitializeUART (native)");
}

}  // namespace gekko::os_debug
