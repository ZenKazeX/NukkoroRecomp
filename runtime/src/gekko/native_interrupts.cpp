// Native interrupt delivery. Contract and rationale: include/gekko/native_interrupts.h
// and docs/native-time-20261005/DESIGN.md.

#include "gekko/native_interrupts.h"

#include <cstdio>
#include <string>

#include "gekko/cpu.h"

namespace gekko {

namespace {

// ---- the entry points the generator routes here (extract/native-owners.txt)
constexpr u32 kInterruptInit = 0x80376198u;     // __OSInterruptInit (from OSInit)
constexpr u32 kSetInterruptMask = 0x8037620Cu;  // SetInterruptMask (OS: apply one mask group)
// The OS handlers of interrupt sources this platform does not have: they stay
// in the OS's handler table, which nothing dispatches through (stage 6).
constexpr u32 kMemInterruptHandler = 0x803769DCu;   // MEMIntrruptHandler (OS interrupts 0-4)
constexpr u32 kResetSwitchHandler = 0x803771F0u;    // __OSResetSWInterruptHandler (OS interrupt 22)
// __OSUnhandledException's report of OS_ERROR_PROTECTION (its switch's case
// 15, a row of its own): it reads the AI / ARAM / DI DMA address registers for
// OSReport and halts. Only MEMIntrruptHandler raises error 15.
constexpr u32 kProtectionReport = 0x803737F8u;
// Delivery continuations, inside SetInterruptMask's extent (its body is not in
// the image): where a guest call made in interrupt context returns, and where
// SelectThread returns when the delivery's reschedule switched nothing.
constexpr u32 kDeliveryReturn = 0x80376210u;
constexpr u32 kRescheduleReturn = 0x80376214u;

// The OS functions the delivery hands control to.
constexpr u32 kSelectThread = 0x803787B0u;   // SelectThread(yield = 0), as __OSReschedule calls it
constexpr u32 kOSLoadContext = 0x80372C68u;  // OSLoadContext: the OS's restore and rfi

// OS globals (low memory) and OSContext (the Dolphin SDK layout the OS uses).
constexpr u32 kCurrentContextPhysical = 0x800000C0u;
constexpr u32 kCurrentContext = 0x800000D4u;
constexpr u32 kFpuContext = 0x800000D8u;
constexpr u32 kContextCr = 0x80u;
constexpr u32 kContextLr = 0x84u;
constexpr u32 kContextCtr = 0x88u;
constexpr u32 kContextXer = 0x8Cu;
constexpr u32 kContextSrr0 = 0x198u;
constexpr u32 kContextSrr1 = 0x19Cu;
constexpr u32 kContextMode = 0x1A0u;
constexpr u32 kContextState = 0x1A2u;
constexpr u32 kContextGqr = 0x1A4u;
constexpr u32 kContextSize = 0x2C8u;
constexpr u16 kContextStateExc = 0x0002u;  // OS_CONTEXT_STATE_EXC: saved by an exception
constexpr u32 kSrr1Mask = 0x87C0FFFFu;     // what the processor keeps in SRR1 (Cpu::raise_exception)

// OS scheduler words, r13-relative (SelectThread 0x803787B0, OSDisableScheduler 0x803784CC).
constexpr u32 kSdaReschedule = 0x5AE8u;     // Reschedule: > 0 disables thread switches
constexpr u32 kSdaRunQueueHint = 0x5AE4u;   // RunQueueHint: a woken thread outranks the running one

// The delivery's guest stack frames, below the interrupted thread's r1:
//   r1 - 8           the vector stub's frame (stwu r1,-8(r1))
//   dispatch frame   kDispatchSize: tag, interrupted context
//   handler frame    kHandlerSize: the exception OSContext the SDK handlers
//                    make current, then tag, source, step, scratch, context
constexpr u32 kStubSize = 8u;
constexpr u32 kDispatchSize = 0x30u;
constexpr u32 kDispatchTag = 0x4E495251u;  // 'NIRQ'
constexpr u32 kDispatchTagOffset = 0x10u;
constexpr u32 kDispatchContextOffset = 0x14u;
constexpr u32 kHandlerSize = 0x300u;
constexpr u32 kHandlerExcOffset = 0x10u;
constexpr u32 kHandlerTag = 0x4E495248u;   // 'NIRH'
constexpr u32 kHandlerTagOffset = kHandlerExcOffset + kContextSize;  // 0x2D8
constexpr u32 kHandlerSourceOffset = kHandlerTagOffset + 4u;
constexpr u32 kHandlerStepOffset = kHandlerTagOffset + 8u;
constexpr u32 kHandlerScratchOffset = kHandlerTagOffset + 12u;  // four words
constexpr u32 kHandlerContextOffset = kHandlerScratchOffset + 16u;
static_assert(kHandlerContextOffset + 4u <= kHandlerSize, "handler frame overflow");

const NativeInterrupts::Address kAddresses[] = {
    {kInterruptInit, "__OSInterruptInit", false},
    {kMemInterruptHandler, "MEMIntrruptHandler", false},
    {kResetSwitchHandler, "__OSResetSWInterruptHandler", false},
    {kProtectionReport, "__OSUnhandledException: the protection-error report", false},
    {kSetInterruptMask, "SetInterruptMask", false},
    {kDeliveryReturn, "native interrupt: guest call return", true},
    {kRescheduleReturn, "native interrupt: reschedule return", true},
};

// The OS's interrupt bookkeeping (__OSInterruptInit): the handler-table
// pointer (InterruptHandlerTable, r13-relative) and the table it points to -
// 32 handler words in the OS globals page -, the global and user masks.
constexpr u32 kSdaInterruptHandlerTable = 0x5A90u;
constexpr u32 kInterruptHandlerTable = 0x80003040u;
constexpr u32 kInterruptHandlerTableBytes = 0x80u;
constexpr u32 kOsGlobalMask = 0x800000C4u;
constexpr u32 kOsUserMask = 0x800000C8u;
// __OSMaskInterrupts' argument in __OSInterruptInit: every OS interrupt (0-26).
constexpr u32 kAllOsInterrupts = 0xFFFFFFE0u;

// The OS interrupt mask bits (OS_INTERRUPTMASK(n) = 0x80000000 >> n).
constexpr u32 os_mask(u32 interrupt) noexcept { return 0x80000000u >> interrupt; }

// __OSDispatchInterrupt's priority table (read at 0x80376880): rows of OS
// interrupt masks ending with 0xFFFFFFFF. The first row holding a pending
// interrupt wins; within it, cntlzw takes the lowest interrupt number. The
// rank is (row, number), smaller first. Read from the game's own data.
constexpr u32 kPriorityTable = 0x805625C8u;  // in the program image
constexpr u32 kPriorityRowLimit = 32u;

u32 os_rank(const GuestMemory& memory, u32 interrupt) {
    const u32 bit = os_mask(interrupt);
    for (u32 row = 0; row < kPriorityRowLimit; ++row) {
        const u32 mask = memory.peek32(kPriorityTable + 4u * row);
        if ((mask & bit) != 0u) return (row << 8) | interrupt;
        if (mask == 0xFFFFFFFFu) break;
    }
    return 0xFFFFu;  // the last row holds every interrupt; not reached
}

// A word-aligned address of the game's mapped memory.
bool ram_word(const GuestMemory& memory, u32 address) noexcept {
    return (address & 3u) == 0u && memory.mapped(GcAddr(address), 4u);
}

void stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "native interrupt delivery: " + why);
}

std::string hex(u32 value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

// OSClearContext (0x80372D78): mode and state cleared; a context that owned
// the FPU no longer does.
void os_clear_context(Cpu& c, u32 context) {
    c.mem_write16(context + kContextMode, 0u);
    c.mem_write16(context + kContextState, 0u);
    if (c.mem_read32(kFpuContext) == context) c.mem_write32(kFpuContext, 0u);
}

// OSSetCurrentContext (0x80372B80): the context becomes current; MSR[FP] (live
// and in the context's SRR1) follows FPU ownership; MSR[RI] is set.
void os_set_current_context(Cpu& c, u32 context) {
    c.mem_write32(kCurrentContext, context);
    // The OS also records the context's GameCube physical address; nothing
    // reads it any more (the exception entry is native: os_exception_entry.cpp)
    // but it is the OS's own bookkeeping word, written as the OS writes it.
    c.mem_write32(kCurrentContextPhysical, context & 0x3FFFFFFFu);
    const u32 srr1 = c.mem_read32(context + kContextSrr1);
    if (c.mem_read32(kFpuContext) == context) {
        c.mem_write32(context + kContextSrr1, srr1 | kMsrFP);
        c.set_msr(c.msr() | kMsrRI);
    } else {
        c.mem_write32(context + kContextSrr1, srr1 & ~kMsrFP);
        c.set_msr((c.msr() & ~kMsrFP) | kMsrRI);
    }
}

}  // namespace

u32 NativeInterrupts::os_interrupt(Source source) noexcept {
    switch (source) {
        case Source::Vi: return 24u;     // __OS_INTERRUPT_PI_VI
        case Source::DspDma: return 6u;    // __OS_INTERRUPT_DSP_ARAM
        case Source::Dsp: return 7u;     // __OS_INTERRUPT_DSP_DSP
        case Source::AiDma: return 5u;   // __OS_INTERRUPT_DSP_AI
        case Source::PeFinish: return 19u;      // __OS_INTERRUPT_PI_PE_FINISH
        case Source::GxBreakpoint: return 17u;  // __OS_INTERRUPT_PI_CP
        case Source::Card: return 9u;           // ranked as __OS_INTERRUPT_EXI_0_EXI
        case Source::Count: break;
    }
    return 31u;
}

std::size_t NativeInterrupts::best_pending() const {
    std::size_t best = kSourceCount;
    u32 best_rank = ~0u;
    for (std::size_t i = 0; i < kSourceCount; ++i) {
        if (!status_[i] || !unmasked_[i]) continue;
        const u32 rank = os_rank(cpu_.guest_memory(), os_interrupt(static_cast<Source>(i)));
        if (rank < best_rank) {
            best_rank = rank;
            best = i;
        }
    }
    return best;
}

void NativeInterrupts::clear_exception_context(Cpu& c, u32 frame) {
    os_clear_context(c, frame + kHandlerExcOffset);
}

const NativeInterrupts::Address* NativeInterrupts::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

u32 NativeInterrupts::scratch(u32 frame, u32 index) noexcept {
    return frame + kHandlerScratchOffset + 4u * (index & 3u);
}

u32 NativeInterrupts::exception_context(u32 frame) noexcept { return frame + kHandlerExcOffset; }

// __OSUnmaskInterrupts: prev = global mask (0x800000C4), local = user mask
// (0x800000C8); the groups whose bits go from masked to enabled are applied
// with the new mask (global & ~mask) | local.
u32 NativeInterrupts::unmask_os_interrupts(Cpu& c, u32 mask) {
    const u32 previous = c.mem_read32(kOsGlobalMask);
    const u32 local = c.mem_read32(kOsUserMask);
    u32 changed = mask & (previous | local);
    const u32 global = previous & ~mask;
    c.mem_write32(kOsGlobalMask, global);
    const u32 current = global | local;
    while (changed != 0u) {
        const u32 remaining = set_interrupt_mask(changed, current);
        if (remaining == changed) break;  // bits no group owns (27..31)
        changed = remaining;
    }
    return previous;
}

// __OSMaskInterrupts: prev = global mask, local = user mask; the groups whose
// bits go from enabled to masked are applied with the new mask
// (global | mask) | local.
u32 NativeInterrupts::mask_os_interrupts(Cpu& c, u32 mask) {
    const u32 previous = c.mem_read32(kOsGlobalMask);
    const u32 local = c.mem_read32(kOsUserMask);
    u32 changed = mask & ~(previous | local);
    const u32 global = previous | mask;
    c.mem_write32(kOsGlobalMask, global);
    const u32 current = global | local;
    while (changed != 0u) {
        const u32 remaining = set_interrupt_mask(changed, current);
        if (remaining == changed) break;  // bits no group owns (27..31)
        changed = remaining;
    }
    return previous;
}

void NativeInterrupts::set_handler(Source source, Handler handler) {
    handlers_[index(source)] = std::move(handler);
}

void NativeInterrupts::raise(Source source) noexcept {
    ++stats_.raised[index(source)];
    status_[index(source)] = true;
    update_line();
}

void NativeInterrupts::clear(Source source) noexcept {
    status_[index(source)] = false;
    update_line();
}

void NativeInterrupts::update_line() noexcept {
    bool pending = false;
    for (std::size_t i = 0; i < kSourceCount; ++i) pending = pending || (status_[i] && unmasked_[i]);
    cpu_.set_native_interrupt(pending);
}

void NativeInterrupts::reset() noexcept {
    status_.fill(false);
    unmasked_.fill(false);
    unmasked_[index(Source::Card)] = true;  // no OS mask group owns it
    update_line();
}

void NativeInterrupts::install() {
    Cpu& cpu = cpu_;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, std::string("native interrupts: ") + name);
    };
    own(kInterruptInit, "__OSInterruptInit", [this](Cpu& c) { interrupt_init(c); });
    own(kSetInterruptMask, "SetInterruptMask", [this](Cpu& c) {
        ++stats_.mask_calls;
        c.gpr[3] = set_interrupt_mask(c.gpr[3], c.gpr[4]);
    });
    own(kDeliveryReturn, "guest call return", [this](Cpu& c) { callback_return(c); });
    own(kRescheduleReturn, "reschedule return", [this](Cpu& c) { resume(c); });
    own(kMemInterruptHandler, "MEMIntrruptHandler", [](Cpu& c) {
        stop(c, "MEMIntrruptHandler (OS interrupt " + std::to_string(c.gpr[3]) +
                    ") was entered: a memory-protection interrupt does not exist on this platform (no memory "
                    "interface, nothing dispatches through the OS's handler table)");
    });
    own(kResetSwitchHandler, "__OSResetSWInterruptHandler", [](Cpu& c) {
        stop(c, "__OSResetSWInterruptHandler (OS interrupt " + std::to_string(c.gpr[3]) +
                    ") was entered: the PC has no reset switch and nothing dispatches through the OS's handler "
                    "table");
    });
    own(kProtectionReport, "__OSUnhandledException: the protection-error report", [](Cpu& c) {
        stop(c, "__OSUnhandledException reached its memory-protection report (error 15): only MEMIntrruptHandler "
                "raises that error, and this platform has no memory interface");
    });
    cpu.set_native_interrupt_handler([this](Cpu& c) { deliver(c); });
}

// __OSInterruptInit (0x80376198), once from OSInit. The original set the
// handler-table pointer and cleared the table, cleared the global and user
// masks, wrote the PI interrupt mask (0xF0), masked every OS interrupt
// (__OSMaskInterrupts(0xFFFFFFE0)) and installed ExternalInterruptHandler for
// the external-interrupt exception. The OS's own state is kept; there is no PI
// mask, and no external-interrupt handler: this platform raises no external
// interrupt (every source is a native delivery), so the exception table's
// entry keeps the OS default and the guest dispatch path is unreachable.
void NativeInterrupts::interrupt_init(Cpu& c) {
    c.mem_write32(c.gpr[13] + kSdaInterruptHandlerTable, kInterruptHandlerTable);
    for (u32 offset = 0; offset < kInterruptHandlerTableBytes; offset += 4u) {
        c.mem_write32(kInterruptHandlerTable + offset, 0u);
    }
    c.mem_write32(kOsGlobalMask, 0u);
    c.mem_write32(kOsUserMask, 0u);
    (void)mask_os_interrupts(c, kAllOsInterrupts);
}

// SetInterruptMask(mask, current) (0x8037620C): applies the OS mask group of
// mask's highest set bit and returns mask without that group. `current` is the
// whole OS mask (global | user); a clear bit enables the interrupt.
u32 NativeInterrupts::set_interrupt_mask(u32 mask, u32 current) {
    const auto enabled = [current](u32 interrupt) { return (current & os_mask(interrupt)) == 0u; };
    // cntlzw: the OS interrupt number of mask's highest set bit (32 for none).
    u32 group = 0u;
    while (group < 32u && (mask & (0x80000000u >> group)) == 0u) ++group;
    if (group <= 4u) {
        // MEM_0..MEM_3, MEM_ADDRESS: nothing to apply. They were the memory
        // interface's protection interrupts; this platform has no memory
        // interface (stage 6).
        return mask & 0x07FFFFFFu;
    }
    if (group <= 7u) {
        // DSP_AI, DSP_ARAM, DSP_DSP: native sources.
        unmasked_[index(Source::AiDma)] = enabled(5u);
        unmasked_[index(Source::DspDma)] = enabled(6u);
        unmasked_[index(Source::Dsp)] = enabled(7u);
        update_line();
        return mask & ~0x07000000u;
    }
    if (group == 8u) {
        // AI_AI, the streaming sample-counter interrupt: the native audio has
        // no stream source, so nothing can raise it.
        return mask & ~0x00800000u;
    }
    if (group <= 16u) {
        // EXI channels 0, 1, 2 (9-11, 12-14, 15-16): nothing to apply. The
        // EXI library that set these interrupts' enables is gone (stage 5),
        // and the card's completion is a native source no OS group owns.
        if (group <= 11u) return mask & ~0x00700000u;
        if (group <= 14u) return mask & ~0x000E0000u;
        return mask & ~0x00018000u;
    }
    if (group <= 26u) {
        // PI_VI: the native retrace source (NativeVideo). PI_CP, PI_PE_FINISH:
        // the native GX sources (NativeGxSync); PI_PE_TOKEN has none (the
        // token interrupt is not delivered: nothing in this image handles it).
        unmasked_[index(Source::Vi)] = enabled(24u);
        unmasked_[index(Source::GxBreakpoint)] = enabled(17u);
        unmasked_[index(Source::PeFinish)] = enabled(19u);
        update_line();
        // PI_SI, PI_DI, PI_RSW, PI_ERROR, PI_DEBUG, PI_HSP: nothing to apply -
        // no such source exists on this platform (controller input has no interrupt,
        // the native file API completes before it returns, there is no reset
        // switch, error line, debugger or high-speed port). The processor
        // interface whose mask they programmed is gone (stage 6).
        return mask & ~0x00007FE0u;
    }
    return mask;
}

void NativeInterrupts::deliver(Cpu& c) {
    const std::size_t chosen = best_pending();
    if (chosen == kSourceCount) {
        c.set_native_interrupt(false);
        return;
    }
    if ((c.msr() & kMsrRI) == 0u) {
        // The OS vector treats an exception taken with MSR[RI] clear as
        // unrecoverable; so does this.
        stop(c, "an interrupt became deliverable with MSR[RI] clear at " + hex(c.pc));
        return;
    }
    const u32 context = c.mem_read32(kCurrentContext);
    if (!ram_word(c.guest_memory(), context)) {
        stop(c, "the OS current context " + hex(context) + " is not mapped guest memory");
        return;
    }
    const u32 interrupted_stack = c.gpr[1];
    constexpr u32 kFrames = kStubSize + kDispatchSize + kHandlerSize;
    if (!ram_word(c.guest_memory(), interrupted_stack) || interrupted_stack < kFrames ||
        !c.guest_memory().mapped(GcAddr(interrupted_stack - kFrames), kFrames)) {
        stop(c, "the interrupted stack pointer " + hex(interrupted_stack) + " cannot hold a delivery frame");
        return;
    }
    // Every original handler acknowledges its source before anything else.
    status_[chosen] = false;
    update_line();
    ++stats_.delivered[chosen];

    // ---- the OS context save (the 0x500 vector, then ExternalInterruptHandler)
    for (u32 r = 0; r < 32u; ++r) c.mem_write32(context + 4u * r, c.gpr[r]);
    c.mem_write32(context + kContextCr, c.cr);
    c.mem_write32(context + kContextLr, c.lr);
    c.mem_write32(context + kContextCtr, c.ctr);
    c.mem_write32(context + kContextXer, c.xer);
    c.mem_write32(context + kContextSrr0, c.pc);
    c.mem_write32(context + kContextSrr1, c.msr() & kSrr1Mask);
    c.mem_write16(context + kContextState,
                  static_cast<u16>(c.mem_read16(context + kContextState) | kContextStateExc));
    for (u32 g = 1; g < 8u; ++g) c.mem_write32(context + kContextGqr + 4u * g, c.spr[SPR_GQR0 + g]);
    c.spr[SPR_SPRG0] = c.gpr[4];
    c.enter_native_interrupt();

    // ---- __OSDispatchInterrupt: its frames, OSDisableScheduler
    const u32 stub = interrupted_stack - kStubSize;
    c.mem_write32(stub, interrupted_stack);
    const u32 dispatch = stub - kDispatchSize;
    c.mem_write32(dispatch, stub);
    c.mem_write32(dispatch + kDispatchTagOffset, kDispatchTag);
    c.mem_write32(dispatch + kDispatchContextOffset, context);
    const u32 sda = c.gpr[13];
    c.mem_write32(sda + kSdaReschedule, c.mem_read32(sda + kSdaReschedule) + 1u);

    // ---- the handler's frame and its exception context
    const u32 frame = dispatch - kHandlerSize;
    c.mem_write32(frame, dispatch);
    const u32 exception_context = frame + kHandlerExcOffset;
    os_clear_context(c, exception_context);
    os_set_current_context(c, exception_context);
    c.mem_write32(frame + kHandlerTagOffset, kHandlerTag);
    c.mem_write32(frame + kHandlerSourceOffset, static_cast<u32>(chosen));
    c.mem_write32(frame + kHandlerStepOffset, 0u);
    for (u32 i = 0; i < 4u; ++i) c.mem_write32(scratch(frame, i), 0u);
    c.mem_write32(frame + kHandlerContextOffset, context);
    c.gpr[1] = frame;

    const Handler& handler = handlers_[chosen];
    if (!handler) {
        stop(c, "source " + std::to_string(chosen) + " has no handler");
        return;
    }
    if (!handler(c, frame, 0u) && !c.stopped()) finish(c, frame);
}

void NativeInterrupts::call_guest(Cpu& c, u32 frame, u32 function, u32 r3, u32 r4) {
    ++stats_.guest_calls;
    c.gpr[1] = frame;
    c.gpr[3] = r3;
    c.gpr[4] = r4;
    c.lr = kDeliveryReturn;
    c.leave_to_dispatcher(function, FallbackReason::Yield, kDeliveryReturn);
}

void NativeInterrupts::callback_return(Cpu& c) {
    const u32 frame = c.gpr[1];
    if (!ram_word(c.guest_memory(), frame) || c.mem_read32(frame + kHandlerTagOffset) != kHandlerTag) {
        stop(c, "a guest call returned without its delivery frame (r1 " + hex(frame) + ")");
        return;
    }
    const u32 source = c.mem_read32(frame + kHandlerSourceOffset);
    const u32 step = c.mem_read32(frame + kHandlerStepOffset) + 1u;
    c.mem_write32(frame + kHandlerStepOffset, step);
    if (source >= kSourceCount || !handlers_[source]) {
        stop(c, "a delivery frame names source " + std::to_string(source));
        return;
    }
    if (handlers_[source](c, frame, step)) return;
    if (!c.stopped()) finish(c, frame);
}

void NativeInterrupts::finish(Cpu& c, u32 frame) {
    const u32 context = c.mem_read32(frame + kHandlerContextOffset);
    os_clear_context(c, frame + kHandlerExcOffset);
    os_set_current_context(c, context);
    const u32 dispatch = c.mem_read32(frame);
    if (!ram_word(c.guest_memory(), dispatch) || c.mem_read32(dispatch + kDispatchTagOffset) != kDispatchTag) {
        stop(c, "the delivery's dispatch frame is gone");
        return;
    }
    c.gpr[1] = dispatch;
    const u32 sda = c.gpr[13];
    c.mem_write32(sda + kSdaReschedule, c.mem_read32(sda + kSdaReschedule) - 1u);
    // __OSReschedule: SelectThread only when a woken thread is waiting. It
    // switches straight to that thread (the interrupted one is saved in its
    // context, state EXC) or returns here having switched nothing.
    if (c.mem_read32(sda + kSdaRunQueueHint) != 0u) {
        ++stats_.reschedules;
        c.gpr[3] = 0u;
        c.lr = kRescheduleReturn;
        c.leave_to_dispatcher(kSelectThread, FallbackReason::Yield, kRescheduleReturn);
        return;
    }
    resume(c);
}

void NativeInterrupts::resume(Cpu& c) {
    const u32 dispatch = c.gpr[1];
    if (!ram_word(c.guest_memory(), dispatch) || c.mem_read32(dispatch + kDispatchTagOffset) != kDispatchTag) {
        stop(c, "the reschedule returned without the delivery's dispatch frame (r1 " + hex(dispatch) + ")");
        return;
    }
    c.gpr[3] = c.mem_read32(dispatch + kDispatchContextOffset);
    c.leave_to_dispatcher(kOSLoadContext, FallbackReason::Yield, kRescheduleReturn);
}

void NativeInterrupts::save_state(std::vector<u8>& out) const {
    out.push_back(static_cast<u8>(kSourceCount));
    for (std::size_t i = 0; i < kSourceCount; ++i) {
        out.push_back(status_[i] ? 1u : 0u);
        out.push_back(unmasked_[i] ? 1u : 0u);
    }
}

bool NativeInterrupts::load_state(const u8*& cursor, const u8* end) {
    if (static_cast<std::size_t>(end - cursor) < 1u + 2u * kSourceCount) return false;
    if (*cursor != kSourceCount) return false;  // another build's source set
    ++cursor;
    for (std::size_t i = 0; i < kSourceCount; ++i) {
        status_[i] = *cursor++ != 0u;
        unmasked_[i] = *cursor++ != 0u;
    }
    if (!unmasked_[index(Source::Card)]) return false;  // never saved masked
    update_line();
    return true;
}

}  // namespace gekko
