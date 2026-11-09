#pragma once

// DMAErrorHandler 0x803726D8, owned natively (docs/agents/os-other.md, step 6; removal plan service os.other).
//
// Original (main.dol, PAL GUPP8P): OSInit registers it as the OS error handler of index 1 (machine check): (error r3,
// OSContext* r4, DSISR r5, DAR r6, varargs). It reads HID2 and prints through OSReport
//   "Machine check received\n"
//   "HID2 = 0x%x   SRR1 = 0x%x\n"                        (HID2, context->srr1 at +0x19C)
// and then branches on the DMA / locked-cache error bits (HID2 & 0x00F00000, SRR1 & 0x00200000):
//   - not a DMA error: "Machine check was not DMA/locked cache related\n", OSDumpContext, PPCHalt (never returns);
//   - a DMA error: it prints the error lines, writes HID2 back (clearing the errors) and RETURNS.
//
// EXPLICIT DIFFERENCE FROM THE ORIGINAL (not equivalence): there is no host machine check, so no DMA error exists to
// recover from, and the recovery (the HID2 write-back, GameCube hardware behaviour) is not reimplemented. The owner
// prints the two lines the original prints before it branches (byte for byte, to the host debug console, which drops
// them on a retail console as the original's UART does) and stops the run with an ExecutionFault naming HID2, SRR1 and
// the context pointer, for BOTH branches. The original's DMA branch returns; this one stops. The rest of the original's
// output (the branch lines and OSDumpContext's text) is not reproduced.

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}
namespace gekko::os_report {
class DebugConsole;
}

namespace gekko::os_machine_check {

constexpr u32 kDmaErrorHandler = 0x803726D8u;
constexpr u32 kContextSrr1 = 0x19Cu;  // OSContext.srr1
constexpr u32 kContextSrr0 = 0x198u;  // OSContext.srr0
constexpr u32 kUnhandledException = 0x803716BCu;  // the default handler of every exception index (OS_EXCEPTION table)
constexpr u32 kErrorHandlerTable = 0x80586CB0u;   // __OSErrorTable, one word per error index

// The text of the second line, for a HID2 and SRR1 value.
void serve_dma_error_handler(Cpu& cpu, os_report::DebugConsole& console);

// The default exception handler 0x803716BC (the stub that saves the registers into the context and continues in
// OSUnhandledException 0x80373590), entered with r3 = exception index and r4 = the OSContext, as the vector leaves it.
// The original looks the handler up in __OSErrorTable (the FP-exception case, index 6 with SRR1[FE0/FE1 enabled bit
// 0x00100000], uses entry 16), calls it, and RESUMES the context when it returns; with no handler (or SRR1[RI] clear) it prints
// the report and OSDumpContext's dump and halts.
//   - handler 0x803726D8 (what OSInit registers at index 1): the native DMAErrorHandler above;
//   - no handler / non-recoverable: the text the original sends through OSReport before OSDumpContext, then a stop;
//   - any other handler: an ExecutionFault naming its address. EXPLICIT DIFFERENCE: the original calls it and resumes the
//     context through OSLoadContext (SDK, not owned); no program path registers one (OSSetErrorHandler's only caller is
//     OSInit, which registers 0x803726D8 only).
// For every stop the difference from the original is the same: the register stores into the context, the time and
// OSDumpContext text, the per-exception detail lines and PPCHalt are not reproduced; the run stops with an ExecutionFault.
void serve_unhandled_exception(Cpu& cpu, os_report::DebugConsole& console);

struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kDmaErrorHandler, "DMAErrorHandler"},
                                                 {kUnhandledException, "OSUnhandledException entry"}};
        count = 2;
        return kAddresses;
    }
};

void install(Cpu& cpu, os_report::DebugConsole& console);

}  // namespace gekko::os_machine_check
