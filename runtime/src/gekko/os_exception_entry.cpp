// The OS's exception entry, natively (docs/memory-architecture-20261006/README.md).
//
// On the console an exception switched translation off and fetched the OS's
// vector, a copy of __OSExceptionVector (38 instructions, gekko/os_exception_vector.h) that
// __OSExceptionInit installs at every vector with `li r3, <index>` patched at
// +0x68. Running with translation off, the vector reached the current
// OSContext through its physical address (low memory 0xC0), saved r3-r5, CR,
// LR, CTR, XER, SRR0 and SRR1 there, set the context's exception flag and
// returned with translation on into the handler the OS registered for the
// index (__OSExceptionTable at 0x80003000). The system-call vector is
// PPCSync's head instead (0x8037829C, 7 instructions: HID0[ABE] set and
// restored around isync/sync, then rfi).
//
// The game's address window has no physical addresses and no real mode, so
// the vector's work is done here: the same stores, made to the OSContext
// through its address (low memory 0xD4, which OSSetCurrentContext keeps with
// the physical one), and the same registers, SRRs and MSR at handler entry.
// The construction is documented in extract/exception-vectors.csv; before
// entering, the entry checks that the guest really installed that vector (the
// guard the translated vector bodies used to carry), so a vector page the
// guest did not build stops the machine instead of being assumed.

#include <cstdio>
#include <string>

#include "gekko/cpu.h"
#include "gekko/cpu_diag.h"
#include "gekko/os_exception_vector.h"

namespace gekko {

namespace {

constexpr u32 kVectorWords = kOsVectorWords;
constexpr u32 kExceptionTable = 0x80003000u;     // __OSExceptionTable
constexpr u32 kCurrentContext = 0x800000D4u;     // OS_CURRENTCONTEXT (virtual)
constexpr u32 kUnrecoverable = 0x803716BCu;      // the vector's target when SRR1[RI] is clear
constexpr u32 kContextBytes = 0x2C8u;            // sizeof(OSContext)
constexpr u32 kMsrRecoverable = 0x00000002u;

// SDK exception index of a vector (OS_EXCEPTION_*; extract/exception-vectors.csv).
bool os_exception_index(Exception vector, u32& index) noexcept {
    switch (vector) {
        case Exception::SystemReset: index = 0u; return true;
        case Exception::MachineCheck: index = 1u; return true;
        case Exception::Dsi: index = 2u; return true;
        case Exception::Isi: index = 3u; return true;
        case Exception::ExternalInterrupt: index = 4u; return true;
        case Exception::Alignment: index = 5u; return true;
        case Exception::Program: index = 6u; return true;
        case Exception::FpUnavailable: index = 7u; return true;
        case Exception::Decrementer: index = 8u; return true;
        case Exception::SystemCall: index = 9u; return true;
        case Exception::Trace: index = 10u; return true;
        case Exception::PerformanceMonitor: index = 11u; return true;
    }
    return false;
}

}  // namespace

// True when the guest installed the OS's vector for `vector` (the bytes at the vector address equal the construction
// above: os_vectors::page_word, the instructions __OSExceptionInit and __OSInitSystemCall copied).
bool Cpu::os_vector_installed(Exception vector, u32 index) {
    const u32 at = 0x80000000u | static_cast<u32>(vector);
    for (u32 word = 0; word < kVectorWords; ++word) {
        if (memory_.peek32(at + 4u * word) != os_exception_page_word(word, index)) return false;
    }
    return memory_.peek_pointer(at, 4u * kVectorWords) != nullptr;
}

// After the architected entry (SRR0, SRR1, MSR, the interrupt FPR depth): the
// vector's work, then its rfi into the handler. npc is the handler.
void Cpu::enter_os_exception(Exception vector) {
    u32 index = 0u;
    if (!os_exception_index(vector, index) || !os_vector_installed(vector, index)) {
        char detail[192];
        std::snprintf(detail, sizeof(detail),
                      "exception 0x%04X taken at pc 0x%08X, but the guest has not installed the OS's vector "
                      "for it (__OSExceptionInit), so there is no OS entry to perform",
                      static_cast<u32>(vector), pc);
        const bool first = !stopped();
        request_stop(StopReason::ExecutionFault, detail);
        if (first) stop_.exception = static_cast<u32>(vector);
        npc = pc;
        return;
    }
    if (vector == Exception::Decrementer && memory_.peek32(kExceptionTable + 4u * index) == kUnrecoverable) {
        // The program installs no decrementer handler (the native OSInit leaves exception 8 at the OS's default one and
        // the CPU drops the power-on edge): the guest wrote DEC itself (mtdec), so it has alarm code this program
        // does not have. Stop, saying so, instead of entering the "unhandled exception" report.
        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "the decrementer exception was taken at pc 0x%08X, but this program has no decrementer handler: "
                      "no alarm code (OSSetAlarm and its callers) is compiled, and the guest armed DEC itself with mtdec",
                      pc);
        const bool first = !stopped();
        request_stop(StopReason::ExecutionFault, detail);
        if (first) stop_.exception = static_cast<u32>(vector);
        npc = pc;
        return;
    }
    ++cpu_diag().os_exception_entries;
    const u32 context = memory_.peek32(kCurrentContext);
    if (context == 0u || memory_.peek_pointer(context, kContextBytes) == nullptr) {
        char detail[192];
        std::snprintf(detail, sizeof(detail),
                      "exception 0x%04X at pc 0x%08X: the current OSContext 0x%08X (low memory 0xD4) is not "
                      "mapped guest memory",
                      static_cast<u32>(vector), pc, context);
        request_stop(StopReason::ExecutionFault, detail);
        npc = pc;
        return;
    }
    if (vector == Exception::SystemCall) {
        // PPCSync's head: HID0[ABE] set, isync, sync, HID0 restored, rfi.
        gpr[9] = spr[SPR_HID0];
        gpr[10] = gpr[9] | 8u;
        spr[SPR_HID0] = gpr[10];
        spr[SPR_HID0] = gpr[9];
        return_from_os_vector();
        return;
    }
    spr[SPR_SPRG0] = gpr[4];
    memory_.write32(context + 0x00Cu, gpr[3]);
    memory_.write32(context + 0x010u, gpr[4]);
    memory_.write32(context + 0x014u, gpr[5]);
    memory_.write16(context + 0x1A2u, static_cast<u16>(memory_.read16(context + 0x1A2u) | 2u));
    memory_.write32(context + 0x080u, cr);
    memory_.write32(context + 0x084u, lr);
    memory_.write32(context + 0x088u, ctr);
    memory_.write32(context + 0x08Cu, xer);
    memory_.write32(context + 0x198u, spr[SPR_SRR0]);
    memory_.write32(context + 0x19Cu, spr[SPR_SRR1]);
    const u32 saved_srr1 = spr[SPR_SRR1];
    spr[SPR_SRR1] = msr_ | kMsrIR | kMsrDR;
    gpr[3] = index;
    gpr[4] = memory_.read32(kCurrentContext);
    // rlwinm. r5, r5, 0, 30, 30: the interrupted MSR's RI bit, recorded in CR0.
    gpr[5] = saved_srr1 & kMsrRecoverable;
    set_cr_field(0, (gpr[5] != 0u ? 4u : 2u) | ((xer & kXerSO) != 0u ? 1u : 0u));
    gpr[5] = gpr[5] != 0u ? memory_.read32(kExceptionTable + 4u * index) : kUnrecoverable;
    spr[SPR_SRR0] = gpr[5];
    return_from_os_vector();
}

// The vector's closing rfi: the architected MSR bits from SRR1, the interrupt
// FPR bookkeeping, and execution at SRR0.
void Cpu::return_from_os_vector() noexcept {
    const u32 saved = spr[SPR_SRR1];
    set_msr(((msr_ & ~0x87c0ff73u) | (saved & 0x87c0ff73u)) & ~kMsrPOW);
    restore_interrupt_fpr();
    npc = spr[SPR_SRR0] & ~3u;
}

// sc at `address`: the system-call exception, which resumes after the sc.
void Cpu::system_call(u32 address) {
    pc = address;
    exception_entry(Exception::SystemCall);
    spr[SPR_SRR0] = address + 4u;
    note_exception_resume(address + 4u);
    enter_os_exception(Exception::SystemCall);
}

}  // namespace gekko
