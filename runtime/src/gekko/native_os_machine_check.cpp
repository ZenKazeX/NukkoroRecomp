// The native owner of DMAErrorHandler (gekko/native_os_machine_check.h).

#include "gekko/native_os_machine_check.h"

#include "gekko/cpu.h"
#include "gekko/native_os_debug.h"
#include "gekko/native_os_report.h"

#include <cstdio>
#include <string>

namespace gekko::os_machine_check {

namespace {
// One OSReport call: the text goes to the debug console unless the FILE already failed (a console without a UART).
void print(Cpu& cpu, os_report::DebugConsole& console, const std::string& text) {
    if (console.file_failed(0u)) return;
    const bool enabled = (os_debug::console_type(cpu) & os_debug::kDevelopmentBit) != 0u;
    console.write(text, enabled);
    if (!enabled) console.fail_file(0u);
}
}  // namespace

void serve_dma_error_handler(Cpu& cpu, os_report::DebugConsole& console) {
    const u32 context = cpu.gpr[4];
    const u32 hid2 = cpu.spr[SPR_HID2];
    const u32 srr1 = cpu.mem_read32(context + kContextSrr1);
    print(cpu, console, "Machine check received\n");
    char line[64];
    std::snprintf(line, sizeof(line), "HID2 = 0x%x   SRR1 = 0x%x\n", hid2, srr1);
    print(cpu, console, line);
    char detail[160];
    std::snprintf(detail, sizeof(detail),
                  "DMAErrorHandler: machine check (HID2 0x%08X, SRR1 0x%08X, context 0x%08X); the host raises none, the "
                  "original's recovery is not reimplemented",
                  hid2, srr1, context);
    cpu.request_stop(StopReason::ExecutionFault, detail);
}

void serve_unhandled_exception(Cpu& cpu, os_report::DebugConsole& console) {
    const u32 error = cpu.gpr[3] & 0xFFu;
    const u32 context = cpu.gpr[4];
    const u32 srr0 = cpu.mem_read32(context + kContextSrr0);
    const u32 srr1 = cpu.mem_read32(context + kContextSrr1);
    u32 index = error;
    if (error == 6u && (srr1 & 0x00100000u) != 0u && cpu.mem_read32(kErrorHandlerTable + 4u * 16u) != 0u) index = 16u;
    const u32 handler = (srr1 & 2u) != 0u ? cpu.mem_read32(kErrorHandlerTable + 4u * index) : 0u;
    char detail[256];
    if (handler == kDmaErrorHandler) {
        cpu.gpr[3] = index;
        serve_dma_error_handler(cpu, console);
        return;
    }
    if (handler != 0u) {
        std::snprintf(detail, sizeof(detail),
                      "OSUnhandledException: error %u has the handler 0x%08X registered (SRR0 0x%08X, SRR1 0x%08X, "
                      "context 0x%08X); only DMAErrorHandler is native, the original's call and context resume are not "
                      "reimplemented",
                      error, handler, srr0, srr1, context);
        cpu.request_stop(StopReason::ExecutionFault, detail);
        return;
    }
    char line[64];
    std::snprintf(line, sizeof(line), (srr1 & 2u) == 0u ? "Non-recoverable Exception %d" : "Unhandled Exception %d", error);
    print(cpu, console, line);
    print(cpu, console, "\n");
    std::snprintf(detail, sizeof(detail),
                  "%s: exception %u (SRR0 0x%08X, SRR1 0x%08X, context 0x%08X); the original's dump is not reproduced",
                  (srr1 & 2u) == 0u ? "non-recoverable exception" : "unhandled exception", error, srr0, srr1, context);
    cpu.request_stop(StopReason::ExecutionFault, detail);
}

void install(Cpu& cpu, os_report::DebugConsole& console) {
    os_report::DebugConsole* const sink = &console;
    cpu.install_hle(kDmaErrorHandler, [sink](Cpu& c) {
        serve_dma_error_handler(c, *sink);
        return true;
    }, "DMAErrorHandler (native)");
    cpu.install_hle(kUnhandledException, [sink](Cpu& c) {
        serve_unhandled_exception(c, *sink);
        return true;
    }, "OSUnhandledException entry (native)");
}

}  // namespace gekko::os_machine_check
