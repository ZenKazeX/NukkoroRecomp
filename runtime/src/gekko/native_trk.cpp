// The native owners of MetroTRK's remaining entries (gekko/native_trk.h).

#include "gekko/native_trk.h"

#include "gekko/cpu.h"

#include <string>

namespace gekko::trk {

void serve_db_init(Cpu& cpu) {
    cpu.mem_write32(cpu.gpr[13] + kSdaDbInterface, kDbInterface);
    cpu.mem_write32(kDbExceptionHookPhysical, kDbExceptionDestinationPhysical);
    cpu.mem_write32(cpu.gpr[13] + kSdaDbVerbosity, 1u);
}

namespace {
void nub_is_up(Cpu& cpu, const char* which) {
    cpu.request_stop(StopReason::ExecutionFault,
                     std::string("TRK console ") + which + ": the debugger nub is up, which nothing in this program starts");
}
}  // namespace

void serve_console_close(Cpu& cpu) {
    if (cpu.mem_read32(kNubState) != 0u) {
        nub_is_up(cpu, "close");
        return;
    }
    cpu.gpr[3] = 1u;
}

void serve_console_write(Cpu& cpu) {
    if (cpu.mem_read8(kConsoleEnabled) == 0u || cpu.mem_read32(kNubState) == 0u) {
        cpu.gpr[3] = 1u;
        return;
    }
    nub_is_up(cpu, "write");
}

void serve_console_read(Cpu& cpu) {
    if (cpu.mem_read8(kConsoleEnabled) == 0u || cpu.mem_read32(kNubState) == 0u) {
        cpu.gpr[3] = 1u;
        return;
    }
    nub_is_up(cpu, "read");
}

void install(Cpu& cpu) {
    static constexpr struct {
        u32 address;
        void (*serve)(Cpu&);
        const char* name;
    } kOwners[] = {{kDbInit, serve_db_init, "DBInit (native)"},
                   {kConsoleClose, serve_console_close, "TRK console close (native)"},
                   {kConsoleWrite, serve_console_write, "TRK console write (native)"},
                   {kConsoleRead, serve_console_read, "TRK console read (native)"}};
    for (const auto& owner : kOwners) {
        const auto serve = owner.serve;
        cpu.install_hle(owner.address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, owner.name);
    }
}

}  // namespace gekko::trk
