// The native owners of OSGetConsoleType and the empty DVD callback (gekko/native_os_misc.h).

#include "gekko/native_os_misc.h"

#include "gekko/cpu.h"
#include "gekko/native_os_report.h"

namespace gekko::os_misc {

void serve_get_console_type(Cpu& cpu) {
    u32 type = 0u;
    const u32 info = cpu.mem_read32(cpu.gpr[13] + kBootInfoCell);
    if (info != 0u) type = cpu.mem_read32(info + kConsoleTypeOffset);
    cpu.gpr[3] = type != 0u ? type : kRetailConsole;
}

void serve_load_cell_5dd0(Cpu& cpu) { cpu.gpr[3] = cpu.mem_read32(cpu.gpr[13] + kCell5DD0); }

void serve_report_string(Cpu& cpu, os_report::DebugConsole& console) {
    // The original: r4 = r3; crclr cr1eq; r3 = r13 + 0x26DC ("%s\n"); bl OSReport. The native OSReport formats the same line.
    cpu.gpr[4] = cpu.gpr[3];
    cpu.gpr[3] = cpu.gpr[13] + kReportStringFormat;
    cpu.set_cr_field(1, cpu.cr_field(1) & ~2u);
    os_report::serve_os_report(cpu, console);
}

void install(Cpu& cpu, os_report::DebugConsole& console) {
    os_report::DebugConsole* const sink = &console;
    cpu.install_hle(kGetConsoleType, [](Cpu& c) {
        serve_get_console_type(c);
        return true;
    }, "OSGetConsoleType (native)");
    cpu.install_hle(kLoadCell5DD0, [](Cpu& c) {
        serve_load_cell_5dd0(c);
        return true;
    }, "r13+0x5DD0 getter (native)");
    cpu.install_hle(kReportString, [sink](Cpu& c) {
        serve_report_string(c, *sink);
        return true;
    }, "OSReport(\"%s\n\", r3) (native)");
    cpu.install_hle(kEmptyCallback, [](Cpu&) { return true; }, "empty callback 0x80378320 (native)");
}

}  // namespace gekko::os_misc
