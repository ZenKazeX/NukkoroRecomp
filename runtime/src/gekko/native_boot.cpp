// The program's start-up, native. Contract and rationale: include/gekko/native_boot.h and docs/agents/native-boot.md.

#include "gekko/native_boot.h"

#include <cstdio>
#include <string>

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_guest_scheduler.h"
#include "gekko/native_interrupts.h"
#include "gekko/native_os_threads.h"
#include "gekko/os_exception_vector.h"

namespace gekko {

namespace {

// ---- the original __start's entry and return points (extract/native-owners.txt)
constexpr u32 kStart = 0x80003154u;
constexpr u32 kOsInit = 0x80370E68u;  // OSInit: native; the game calls it again (0x80051028), when it is a no-op
constexpr u32 kAfterConstructors = 0x800032A0u;  // ... of `bl 0x803796AC` (the constructors' wrapper)
constexpr u32 kAfterMain = 0x800032ACu;          // ... of `bl main`; the original then tail-called exit()

// ---- the guest functions it runs
constexpr u32 kConstructors = 0x803796CCu;  // liveness-roots: value-only
constexpr u32 kWalkerReturn = 0x803796FCu;  // the walker's return point after each constructor (`addi r31, r31, 4`)
constexpr u32 kConstructorTable = 0x804AAC60u;  // the walker's table: function pointers, ended by a 0 word
constexpr u32 kMain = 0x800510C0u;
constexpr u32 kExit = 0x803A3B04u;

// ---- the registers __init_registers set
constexpr u32 kStackBase = 0x8060C5F0u;  // r1: the stack's top
constexpr u32 kSdaRw = 0x805EC500u;      // r13: the small-data base (.sdata / .sbss)
constexpr u32 kSdaRo = 0x805FA780u;      // r2: the read-only small-data base (.sdata2)

// ---- the low-memory boot block and the boot information
constexpr u32 kDebuggerExceptionMask = 0x80000044u;  // __start zeroes it (the debugger's per-exception mask word)
constexpr u32 kBi2Pointer = 0x800000F4u;             // the loader's pointer to the disc's second boot block
constexpr u32 kBi2ArgumentsOffset = 8u;              // BI2 + 8: the offset of the argument block (0: none)
constexpr u32 kBi2DebugMonitor = 0xCu;               // BI2 + 0xC: 2 / 3 = MetroTRK, 4 = MetroTRK_BBA
constexpr u32 kBootFlags = 0x800030E4u;              // u16: __start asks OSResetSystem when (flags & 0xEEF) == 0xEEF
constexpr u32 kBootState = 0x800030E6u;              // u16: bit 15 = set by OSInit, low 15 bits = the boot kind

// ---- DBInit's state the guest still reads: OSExceptionInit asks DBIsExceptionMarked(exception) = bit of the word at
// __DBInterface + 4, through the pointer in the small-data area.
constexpr u32 kDbInterface = 0x80000040u;
constexpr u32 kSdaDbInterface = 0x5A18u;  // r13-relative: __DBInterface

// ---- OSInit's native port (docs/agents/native-boot.md has the disassembly and the contract evidence)
constexpr u32 kLowMemory = 0x80000000u;
constexpr u32 kBootInfoArenaLo = 0x80000030u;  // the loader's arena bounds (0: use the defaults)
constexpr u32 kBootInfoArenaHi = 0x80000034u;
constexpr u32 kBusTimeHi = 0x800030D8u;        // OSGetTime's base (the bus-clock offset the loader left)
constexpr u32 kBusTimeLo = 0x800030DCu;
constexpr u32 kSimulatedMemoryPointer = 0x800030F0u;
constexpr u32 kBootInfoCopy = 0x80586C90u;     // OSInit's 0x1C-byte copy of the loader's block
constexpr u32 kStackTop = 0x8060C5F0u;         // the arena starts above the stack
constexpr u32 kDefaultArenaHi = 0x817A0000u;
constexpr u32 kExceptionTable = 0x80003000u;   // __OSExceptionTable: one handler per exception index
constexpr u32 kExceptionVectorCount = 15u;
// The vector address of OS exception index 0..14 (the table at 0x8056121C in the original).
constexpr u32 kExceptionVectorOffset[kExceptionVectorCount] = {0x100u, 0x200u, 0x300u, 0x400u, 0x500u, 0x600u,
                                                                0x700u, 0x800u, 0x900u, 0xC00u, 0xD00u, 0xF00u,
                                                                0x1300u, 0x1400u, 0x1700u};
constexpr u32 kUnhandledException = 0x803716BCu;    // the default handler of every exception index
constexpr u32 kPiHandler = 0x803771F0u;             // interrupt 22's handler
constexpr u32 kInterruptHandlerTable = 0x80003040u;
constexpr u32 kContextPointer = 0x800000D4u;    // OS_CURRENTCONTEXT (virtual)
constexpr u32 kContextPhysical = 0x800000C0u;   // OS_CURRENTCONTEXT_PHYS
constexpr u32 kFpuContext = 0x800000D8u;        // OS_FPU_CONTEXT
constexpr u32 kThreadQueueHead = 0x800000DCu;   // the active-thread queue's head, the tail at +4
constexpr u32 kCurrentThread = 0x800000E4u;     // OS_CURRENTTHREAD
constexpr u32 kThreadBlock = 0x80587038u;       // the OS's thread state; the default thread is at +0x418
constexpr u32 kDefaultThread = 0x80587450u;
constexpr u32 kStackEnd = 0x805FC5ECu;          // the default thread's stack end (the guard word)
constexpr u32 kStackGuardMagic = 0xDEADBABEu;
// small-data offsets (r13-relative)
constexpr u32 kSdaOsInitialized = 0x5A40u;
constexpr u32 kSdaBootInfo = 0x5A20u;
constexpr u32 kSdaDebugKind = 0x5A24u;
constexpr u32 kSdaFlag5B54 = 0x5B54u;
constexpr u32 kSdaDebugWord = 0x5C58u;
constexpr u32 kSdaTimeHi = 0x5A50u;
constexpr u32 kSdaTimeLo = 0x5A54u;
constexpr u32 kSdaArenaLo = 0x26E8u;
constexpr u32 kSdaArenaHi = 0x5A70u;
constexpr u32 kSdaExceptionTable = 0x5A44u;
constexpr u32 kSdaAlarmQueue = 0x5A58u;
constexpr u32 kSdaRunQueueWords = 0x5AE0u;
constexpr u32 kSdaThreadFlag = 0x5AE8u;
constexpr u32 kMsrFpBit = 0x00002000u;
constexpr u32 kMsrRecoverableBit = 0x00000002u;
constexpr u32 kMsrExternalInterrupt = 0x00008000u;
constexpr u32 kHid0SpeculativeDisable = 0x200u;  // SPD
constexpr u32 kHid2PairedSinglesOff = 0xBFFFFFFFu;
constexpr u32 kFpscrNonIeee = 0x4u;              // mtfsb1 29
constexpr u32 kMemAddressInterrupt = 0x08000000u;  // OS interrupt 4

// The entries the original OSInit called that are native owners of this program: served directly, in order.
constexpr u32 kOwnedInterruptInit = 0x80376198u;  // __OSInterruptInit
constexpr u32 kOwnedExiInit = 0x803B4CFCu;        // EXIInit
constexpr u32 kOwnedInitSram = 0x803776F4u;       // __OSInitSram
constexpr u32 kOwnedInitAudio = 0x80372260u;      // __OSInitAudioSystem

const NativeBoot::Address kAddresses[] = {
    {kStart, "__start", false},
    {kOsInit, "OSInit", false},
    {kAfterConstructors, "__start: the constructors returned", true},
    {kWalkerReturn, "__start: a constructor returned to the table walk", true},
    {kAfterMain, "__start: main returned", true},
};

void stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "native boot: " + why);
}

const char* hex(u32 value) {
    static thread_local char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

}  // namespace

void NativeBoot::use_original_boot() noexcept {
    original_ = true;
    cpu_.set_decrementer_delivery(true);  // the original OSInit installs the alarm handler
}

const NativeBoot::Address* NativeBoot::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

void NativeBoot::install() {
    Cpu& cpu = cpu_;
    // No alarm in this program: the decrementer's sign edge has no consumer (see os_init).
    cpu.set_decrementer_delivery(false);
    const auto own = [this, &cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [this, serve](Cpu& c) {
            if (original_) return false;  // the reference runner's --original-boot: the interpreter runs the guest body
            serve(c);
            return true;  // an owner never declines
        }, std::string("native boot: ") + name);
    };
    own(kStart, "__start", [this](Cpu& c) { start(c); });
    own(kOsInit, "OSInit", [this](Cpu& c) { os_init(c); });
    own(kAfterConstructors, "the constructors returned", [this](Cpu& c) { after_constructors(c); });
    own(kWalkerReturn, "a constructor returned", [this](Cpu& c) { after_constructor(c); });
#if defined(SHADOW_RENDER_LIFTED)
    // The guest __init_cpp is not in the lifted build (extract/native-owners-r2.txt): the table walk above replaces it, and its only
    // caller (the wrapper 0x803796AC, called from __start's owner) is never run. Entering it is a fault.
    cpu.install_hle(kConstructors, [](Cpu& c) {
        stop(c, "the guest constructor walker was entered (the native boot walks the table itself)");
        return true;
    }, "native boot: __init_cpp (never entered)");
#endif
    own(kAfterMain, "main returned", [this](Cpu& c) { after_main(c); });
}

void NativeBoot::start(Cpu& c) {
    if (phase_ != Phase::Idle) {
        stop(c, "__start entered again (phase " + std::to_string(static_cast<int>(phase_)) + ")");
        return;
    }
    // The boot information. The original read the debug-monitor kind and the argument block from it; this program
    // has no debug monitor (InitMetroTRK is a refusing owner) and no argument parser, so a loader that asks for
    // either is refused rather than silently ignored.
    const u32 bi2 = c.mem_read32(kBi2Pointer);
    if (bi2 == 0u || !c.guest_memory().mapped(GcAddr(bi2), 16u)) {
        stop(c, std::string("the boot information pointer ") + hex(bi2) + " (0x800000F4) is not a readable block");
        return;
    }
    const u32 monitor = c.mem_read32(bi2 + kBi2DebugMonitor);
    if (monitor == 2u || monitor == 3u || monitor == 4u) {
        stop(c, "the boot information names debug monitor " + std::to_string(monitor) +
                    ", and this program has none (the original entered InitMetroTRK)");
        return;
    }
    if (c.mem_read32(bi2 + kBi2ArgumentsOffset) != 0u) {
        stop(c, "the boot information passes program arguments, and this program has no argument parser");
        return;
    }

    // __init_registers: the stack and the two small-data bases. Everything else the original zeroed is a value no
    // code reads (the next instructions load or overwrite it) and the CPU's reset state is zero anyway.
    c.gpr[2] = kSdaRo;
    c.gpr[13] = kSdaRw;
    // __init_hardware (0x803725F4): HID0 |= 0x800 (ICFI, instruction-cache flash invalidate; nothing clears it, so the
    // original boot ends with it set). HID2's paired-single enables, GQR0-7 = 0 and the zeroed FPR file are the CPU's
    // reset state; the caches are on in it.
    c.spr[SPR_HID0] |= 0x800u;
    // The original's first frame: `stwu r1, -8(r1)` and two -1 words, the terminator of the call chain.
    c.gpr[1] = kStackBase - 8u;
    c.mem_write32(kStackBase - 8u, 0xFFFFFFFFu);
    c.mem_write32(kStackBase - 4u, 0xFFFFFFFFu);
    // main(argc, argv): no arguments.
    c.gpr[14] = 0u;
    c.gpr[15] = 0u;

    c.mem_write32(kDebuggerExceptionMask, 0u);
    // DBInit: the pointer OSExceptionInit's DBIsExceptionMarked reads. Its other two stores (the hook word at
    // 0x80000048 and the verbosity flag in the small-data area) have no reader (docs/agents/native-boot.md).
    c.mem_write32(kSdaRw + kSdaDbInterface, kDbInterface);

    phase_ = Phase::OsInit;
    os_init(c);
    if (c.stopped()) return;
    after_os_init(c);
}

void NativeBoot::os_init(Cpu& c) {
    const u32 sda = c.gpr[13];
    if (c.mem_read32(sda + kSdaOsInitialized) != 0u) return;  // OSInit ran already
    c.mem_write32(sda + kSdaOsInitialized, 1u);
    // OSGetTime: the time base plus the loader's bus-time offset.
    {
        const u64 base = (static_cast<u64>(c.mem_read32(kBusTimeHi)) << 32) | c.mem_read32(kBusTimeLo);
        const u64 time = c.time_base() + base;
        c.mem_write32(sda + kSdaTimeHi, static_cast<u32>(time >> 32));
        c.mem_write32(sda + kSdaTimeLo, static_cast<u32>(time));
    }
    // The copy of the loader's 0x1C-byte block OSInit keeps (0x80586C90): the block when it points into RAM, else cleared.
    {
        const u32 source = c.mem_read32(kSimulatedMemoryPointer);
        if (kLowMemory > source) {
            c.mem_write32(kBootInfoCopy, 0u);
        } else {
            for (u32 offset = 0; offset < 0x1Cu; offset += 4u) {
                c.mem_write32(kBootInfoCopy + offset, c.mem_read32(source + offset));
            }
        }
    }
    // The processor modes OSInit set: the performance monitor stays off (reset state), HID0[SPD], FPSCR[NI].
    c.spr[SPR_HID0] |= kHid0SpeculativeDisable;
    c.fpscr |= kFpscrNonIeee;

    // The boot information the OS keeps (start() validated the pointer and the debug-monitor kind).
    const u32 bi2 = c.mem_read32(kBi2Pointer);
    c.mem_write32(sda + kSdaBootInfo, kLowMemory);
    c.mem_write32(sda + kSdaDebugKind, bi2 + kBi2DebugMonitor);
    c.mem_write32(sda + kSdaDebugWord, c.mem_read32(bi2 + 0x24u));
    c.mem_write8(0x800030E8u, static_cast<u8>(c.mem_read32(bi2 + kBi2DebugMonitor)));
    c.mem_write8(0x800030E9u, static_cast<u8>(c.mem_read32(sda + kSdaDebugWord)));
    c.mem_write32(sda + kSdaFlag5B54, 1u);

    // The memory arena: the loader's bounds, else the defaults (the low bound above the stack, 32-byte aligned).
    {
        const u32 lo = c.mem_read32(kBootInfoArenaLo);
        c.mem_write32(sda + kSdaArenaLo, lo != 0u ? lo : ((kStackTop + 0x1Fu) & ~0x1Fu));
        const u32 hi = c.mem_read32(kBootInfoArenaHi);
        c.mem_write32(sda + kSdaArenaHi, hi != 0u ? hi : kDefaultArenaHi);
    }

    // __OSExceptionInit: every exception index gets the default handler and the OS's vector page; __OSInitSystemCall's
    // PPCSync head is part of page 9.
    c.mem_write32(sda + kSdaExceptionTable, kExceptionTable);
    for (u32 index = 0; index < kExceptionVectorCount; ++index) {
        const u32 page = kLowMemory | kExceptionVectorOffset[index];
        for (u32 word = 0; word < kOsVectorWords; ++word) {
            c.mem_write32(page + 4u * word, os_exception_page_word(word, index));
        }
        c.mem_write32(kExceptionTable + 4u * index, kUnhandledException);
    }
    // __OSInitAlarm: the alarm queue is empty. Its other work, installing the decrementer exception's alarm handler
    // (0x80371E14), is not done: no alarm caller is compiled in this image (OSSetAlarm and the DVD / card code that
    // called it are not part of the program), so the handler could only find the queue empty and resume the code.
    // The CPU does not deliver the decrementer exception (Cpu::set_decrementer_delivery, below): exception 8 keeps the
    // default handler, which a guest that arms the decrementer (mtdec) would reach: the machine then stops, visibly.
    c.mem_write32(sda + kSdaAlarmQueue, 0u);
    c.mem_write32(sda + kSdaAlarmQueue + 4u, 0u);
    c.mem_write32(0x800030C8u, 0u);
    c.mem_write32(0x800030CCu, 0u);
    c.mem_write32(0x800030D0u, 0u);

    // The interrupt system (an owner of this program), then interrupt 22's handler (the table pointer is the owner's).
    (void)c.serve_owner(kOwnedInterruptInit);
    c.mem_write32(kInterruptHandlerTable + 4u * 22u, kPiHandler);
    // The floating-point-unavailable handler, and no FPU context owner yet.
    // The reference build (guest threads, the OS's lazy FPU switch) keeps the OS handler; the lifted build has none: MSR[FP] is
    // never cleared there (os_threads::single_thread_configuration), so exception 7 is an unhandled exception.
#if defined(SHADOW_RENDER_LIFTED)
    c.mem_write32(kExceptionTable + 4u * 7u, kUnhandledException);
#else
    c.mem_write32(kExceptionTable + 4u * 7u, guest_scheduler::fp_unavailable_handler());
#endif
    c.mem_write32(kFpuContext, 0u);

    // The external-interface and settings services, as owners.
    (void)c.serve_owner(kOwnedExiInit);
    (void)c.serve_owner(kOwnedInitSram);

    // __OSThreadInit: the default thread is the running one.
    {
        const u32 base = kThreadBlock;
        c.mem_write16(base + 0x6E0u, 2u);
        c.mem_write16(base + 0x6E2u, 1u);
        c.mem_write32(base + 0x6E4u, 0u);
        c.mem_write32(base + 0x6E8u, 0x10u);
        c.mem_write32(base + 0x6ECu, 0x10u);
        c.mem_write32(base + 0x6F0u, 0xFFFFFFFFu);
        c.mem_write32(base + 0x708u, 0u);
        c.mem_write32(base + 0x70Cu, 0u);
        c.mem_write32(base + 0x710u, 0u);
        c.mem_write32(kDefaultThread + 0x2E8u, 0u);       // the thread's own queue
        c.mem_write32(kDefaultThread + 0x2E8u + 4u, 0u);
        // OSClearContext, then OSSetCurrentContext, on the default thread's context
        c.mem_write16(kDefaultThread + 0x1A0u, 0u);
        c.mem_write16(kDefaultThread + 0x1A2u, 0u);
        c.mem_write32(kFpuContext, 0u);
        c.mem_write32(kContextPointer, kDefaultThread);
        c.mem_write32(kContextPhysical, kDefaultThread & 0x3FFFFFFFu);
        if (os_threads::single_thread_configuration()) {  // one thread: MSR[FP] stays set (os_set_current_context)
            c.mem_write32(kDefaultThread + 0x19Cu, c.mem_read32(kDefaultThread + 0x19Cu) | kMsrFpBit);
            c.set_msr(c.msr() | kMsrFpBit | kMsrRecoverableBit);
        } else {
            c.mem_write32(kDefaultThread + 0x19Cu, c.mem_read32(kDefaultThread + 0x19Cu) & ~kMsrFpBit);
            c.set_msr((c.msr() & ~kMsrFpBit) | kMsrRecoverableBit);
        }
        c.mem_write32(base + 0x71Cu, kStackTop);
        c.mem_write32(base + 0x720u, kStackEnd);
        c.mem_write32(kStackEnd, kStackGuardMagic);
        c.mem_write32(kCurrentThread, kDefaultThread);
        c.mem_write32(sda + kSdaRunQueueWords, 0u);
        c.mem_write32(sda + kSdaRunQueueWords + 4u, 0u);
        for (u32 priority = 0; priority < 32u; ++priority) {
            c.mem_write32(base + 8u * priority, 0u);
            c.mem_write32(base + 8u * priority + 4u, 0u);
        }
        // the active-thread queue: the default thread alone
        c.mem_write32(kThreadQueueHead, 0u);
        c.mem_write32(kThreadQueueHead + 4u, 0u);
        const u32 tail = c.mem_read32(kThreadQueueHead + 4u);
        if (tail == 0u) c.mem_write32(kThreadQueueHead, kDefaultThread);
        else c.mem_write32(tail + 0x2FCu, kDefaultThread);
        c.mem_write32(kDefaultThread + 0x300u, tail);
        c.mem_write32(kDefaultThread + 0x2FCu, 0u);
        c.mem_write32(kThreadQueueHead + 4u, kDefaultThread);
        c.mem_write32(base + 0x730u, 0u);
        c.mem_write32(base + 0x734u, 0u);
        c.mem_write32(sda + kSdaThreadFlag, 0u);
    }
    (void)c.serve_owner(kOwnedInitAudio);
    // __OSInitMemoryProtection: the memory-interface hardware is lowered (there is none); what stays is the OS's
    // mask word, MEM_ADDRESS (interrupt 4) unmasked: __OSUnmaskInterrupts(0x08000000).
    (void)interrupts_->unmask_os_interrupts(c, kMemAddressInterrupt);
    c.spr[SPR_HID2] &= kHid2PairedSinglesOff;
    // OSEnableInterrupts: the machine takes interrupts from here.
    c.set_msr(c.msr() | kMsrExternalInterrupt);
}

void NativeBoot::after_os_init(Cpu& c) {
    if (phase_ != Phase::OsInit) {
        stop(c, "OSInit's return point entered in phase " + std::to_string(static_cast<int>(phase_)));
        return;
    }
    // `if (!(state & 0x8000) || (state & 0x7FFF) == 1) { if ((flags & 0xEEF) == 0xEEF) OSResetSystem(0, 0, 0); }`
    const u16 state = c.mem_read16(kBootState);
    if ((state & 0x8000u) == 0u || (state & 0x7FFFu) == 1u) {
        const u16 flags = c.mem_read16(kBootFlags);
        if ((flags & 0xEEFu) == 0xEEFu) {
            c.request_stop(StopReason::HleExit,
                           "native boot: __start asked the console to restart (OSResetSystem(0, 0, 0): the boot "
                           "flags 0x800030E4 hold 0xEEF); the PC has no console to reset, the program ends");
            return;
        }
    }
    // The debugger-link flag (set only when the boot information names MetroTRK_BBA) is never set: refused above.

    phase_ = Phase::Constructors;
    c.lr = kAfterConstructors;
    constructors_begin(c);
}

// __init_cpp, instruction for instruction in its effects: the frame (LR saved above it, r31 saved in it), r31 = the table, then
// per slot `mtlr r12; blrl` (LR = 0x803796FC) and `addi r31, r31, 4` until a 0 word, then the frame is popped and the walker
// returns to the wrapper's caller (0x800032A0). The observers of the opt-in boot contract at the walker's entry are served first.
void NativeBoot::constructors_begin(Cpu& c) {
#if !defined(SHADOW_RENDER_LIFTED)
    (void)c.serve_owner(kConstructors);  // the contract observer, when installed (nothing else is installed there)
#endif
    const u32 old_sp = c.gpr[1];
    c.gpr[0] = c.lr;
    c.mem_write32(old_sp + 4u, c.gpr[0]);
    c.gpr[1] = old_sp - 0x10u;
    c.mem_write32(c.gpr[1], old_sp);
    c.mem_write32(c.gpr[1] + 0xCu, c.gpr[31]);
    c.gpr[3] = 0x804B0000u;
    c.gpr[0] = kConstructorTable;
    c.gpr[31] = c.gpr[0];
    constructors_next(c);
}

void NativeBoot::constructors_next(Cpu& c) {
    c.gpr[12] = c.mem_read32(c.gpr[31]);
    // cmplwi r12, 0 -> cr0 (GT or EQ, SO copied from XER)
    const u32 field = (c.gpr[12] != 0u ? 0x4u : 0x2u) | ((c.xer & 0x80000000u) != 0u ? 0x1u : 0u);
    c.cr = (c.cr & 0x0FFFFFFFu) | (field << 28);
    if (c.gpr[12] != 0u) {
        c.lr = kWalkerReturn;
        c.leave_to_dispatcher(c.gpr[12], FallbackReason::Yield, kWalkerReturn);
        return;
    }
    c.gpr[0] = c.mem_read32(c.gpr[1] + 0x14u);
    c.gpr[31] = c.mem_read32(c.gpr[1] + 0xCu);
    c.gpr[1] += 0x10u;
    c.lr = c.gpr[0];
    c.leave_to_dispatcher(c.lr, FallbackReason::Yield, kWalkerReturn);
}

void NativeBoot::after_constructor(Cpu& c) {
    if (phase_ != Phase::Constructors) {
        stop(c, "a constructor returned to the table walk in phase " + std::to_string(static_cast<int>(phase_)));
        return;
    }
    c.gpr[31] += 4u;
    constructors_next(c);
}

void NativeBoot::after_constructors(Cpu& c) {
    if (phase_ != Phase::Constructors) {
        stop(c, "the constructors' return point entered in phase " + std::to_string(static_cast<int>(phase_)));
        return;
    }
    phase_ = Phase::Main;
    c.gpr[3] = c.gpr[14];
    c.gpr[4] = c.gpr[15];
    c.lr = kAfterMain;
    c.leave_to_dispatcher(kMain, FallbackReason::Yield, kAfterMain);
}

void NativeBoot::after_main(Cpu& c) {
    if (phase_ != Phase::Main) {
        stop(c, "main's return point entered in phase " + std::to_string(static_cast<int>(phase_)));
        return;
    }
    // The original `b exit`: r3 is main's result, LR still the return point (exit never returns).
    phase_ = Phase::Exit;
    c.leave_to_dispatcher(kExit, FallbackReason::Yield, kAfterMain);
}

}  // namespace gekko
