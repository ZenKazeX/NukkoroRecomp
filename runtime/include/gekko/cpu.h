#pragma once

// IBM/Motorola PowerPC 750CL ("Gekko") interpreter.
//
// Scope: everything the retail GameCube boot path can execute - the integer and
// branch base, the 750 supervisor surface (SPRs, MSR, BATs, exceptions, time
// base, decrementer), the classic FPU, and the Gekko paired-single extensions
// with GQR-quantised loads/stores.
//
// Correctness rules honoured here:
//   * guest pointers stay 32-bit, guest memory stays big-endian;
//   * CR / XER carry / XER overflow / FPSCR are modelled explicitly;
//   * an instruction this core does not implement stops the machine with a
//     diagnostic instead of silently behaving like a nop.

#include <array>
#include <atomic>
#if defined(_MSC_VER)
#include <intrin.h>
#include <stdlib.h>
#endif
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gekko/game_clock.h"
#include "gekko/gx_sites.h"
#include "gekko/lift_sites.h"
#include "gekko/platform_sites.h"
#include "gekko/guest_memory.h"
#include "gekko/types.h"

namespace gekko {

struct Lifted;

// The GX write-gather pipe's page: the address the game's code forms for it
// (GX_FIFO_BASE, 0xCC008000). Nothing is behind it: every store the image makes
// there is a listed GX site the generator lowered (gekko/gx_sites.h). A store
// that reaches the page while a site sink is installed is unlisted and stops
// the machine (Cpu::unlisted_gx_store).
constexpr u32 kGxPipePageMask = 0xFFFFFF00u;
constexpr u32 kGxPipePage = 0xCC008000u;

// DSISR bits for a data storage interrupt (gekko_user_manual.txt PAGE 192,
// Table 5-3: "no matching PTE ... and no matching BAT" sets DSISR[1]; the
// store bit is DSISR[6] as the PowerPC architecture defines it, which the
// manual defers to for the DSI exception, PAGE 159 Table 4-2 row 00300).
constexpr u32 kDsisrNoTranslation = 0x40000000u;
constexpr u32 kDsisrStore = 0x02000000u;

// MSR bit masks (PowerPC bit numbering has bit 0 as the most significant bit).
constexpr u32 kMsrPOW = 0x00040000u;
constexpr u32 kMsrILE = 0x00010000u;
constexpr u32 kMsrEE = 0x00008000u;
constexpr u32 kMsrPR = 0x00004000u;
constexpr u32 kMsrFP = 0x00002000u;
constexpr u32 kMsrME = 0x00001000u;
constexpr u32 kMsrFE0 = 0x00000800u;
constexpr u32 kMsrSE = 0x00000400u;
constexpr u32 kMsrBE = 0x00000200u;
constexpr u32 kMsrFE1 = 0x00000100u;
constexpr u32 kMsrIP = 0x00000040u;
constexpr u32 kMsrIR = 0x00000020u;
constexpr u32 kMsrDR = 0x00000010u;
constexpr u32 kMsrPM = 0x00000004u;
constexpr u32 kMsrRI = 0x00000002u;
constexpr u32 kMsrLE = 0x00000001u;

// XER bit masks.
constexpr u32 kXerSO = 0x80000000u;
constexpr u32 kXerOV = 0x40000000u;
constexpr u32 kXerCA = 0x20000000u;

// FPSCR bit masks (IBM numbering: bit n is 1u << (31 - n)). The field grouping
// matches mcrfs, gekko_user_manual.txt PAGE 457: field 0 = FX FEX VX OX,
// field 1 = UX ZX XX VXSNAN, field 2 = VXISI VXIDI VXZDZ VXIMZ, field 3 = VXVC
// FR FI C, field 4 = FPCC, field 5 = (reserved) VXSOFT VXSQRT VXCVI, fields
// 6-7 = VE OE UE ZE XE NI RN.
constexpr u32 kFpscrFX = 0x80000000u;      // bit 0, sticky exception summary
constexpr u32 kFpscrFEX = 0x40000000u;     // bit 1, enabled-exception summary
constexpr u32 kFpscrVX = 0x20000000u;      // bit 2, invalid-operation summary
constexpr u32 kFpscrOX = 0x10000000u;      // bit 3
constexpr u32 kFpscrUX = 0x08000000u;      // bit 4
constexpr u32 kFpscrZX = 0x04000000u;      // bit 5
constexpr u32 kFpscrXX = 0x02000000u;      // bit 6
constexpr u32 kFpscrVXSNAN = 0x01000000u;  // bit 7
constexpr u32 kFpscrVXVC = 0x00080000u;    // bit 12
constexpr u32 kFpscrC = 0x00010000u;       // bit 15
constexpr u32 kFpscrFPCC = 0x0000f000u;    // bits 16-19 (FL FG FE FU)
constexpr u32 kFpscrVE = 0x00000080u;      // bit 24
constexpr u32 kFpscrOE = 0x00000040u;      // bit 25
constexpr u32 kFpscrUE = 0x00000020u;      // bit 26
constexpr u32 kFpscrZE = 0x00000010u;      // bit 27
constexpr u32 kFpscrXE = 0x00000008u;      // bit 28
// Non-IEEE mode (bit 29, PAGE 82). OSInit sets it with mtfsb1 29 and the game
// never clears it: denormalised operands and results become zero (PAGE 84-85,
// Tables 2-21/2-22) and fctiw/fctiwz of an invalid operand give 0x80000000.
constexpr u32 kFpscrNI = 0x00000004u;      // bit 29
// Every invalid-operation exception bit VX summarises: VXSNAN VXISI VXIDI
// VXZDZ VXIMZ VXVC (bits 7-12) and VXSOFT VXSQRT VXCVI (bits 21-23).
constexpr u32 kFpscrVXAny = 0x01f80700u;

// HID2 enables (gekko_user_manual.txt PAGE 67, Table 2-6; IBM bit n is
// 1u << (31 - n)). PAGE 90: dcbz_l is illegal when LCE = 0; psq_l, psq_lu,
// psq_st and psq_stu are illegal when LSQE = 0 or PSE = 0; every other paired
// single instruction is illegal when PSE = 0.
constexpr u32 kHid2LSQE = 0x80000000u;  // bit 0
constexpr u32 kHid2WPE = 0x40000000u;   // bit 1
constexpr u32 kHid2PSE = 0x20000000u;   // bit 2
constexpr u32 kHid2LCE = 0x10000000u;   // bit 3

// SRR1 bit 12 of a program exception: an illegal instruction caused it (PowerPC
// OEA program exception; gekko_user_manual.txt PAGE 175, section 4.5.7, defers
// the SRR1 layout to the Programming Environments manual).
constexpr u32 kSrr1IllegalInstruction = 0x00080000u;

enum Spr : u32 {
    SPR_XER = 1,
    SPR_LR = 8,
    SPR_CTR = 9,
    SPR_DSISR = 18,
    SPR_DAR = 19,
    SPR_DEC = 22,
    SPR_SDR1 = 25,
    SPR_SRR0 = 26,
    SPR_SRR1 = 27,
    SPR_SPRG0 = 272,
    SPR_SPRG1 = 273,
    SPR_SPRG2 = 274,
    SPR_SPRG3 = 275,
    SPR_EAR = 282,
    SPR_TBL_READ = 268,
    SPR_TBU_READ = 269,
    SPR_TBL_WRITE = 284,
    SPR_TBU_WRITE = 285,
    SPR_PVR = 287,
    SPR_IBAT0U = 528,
    SPR_IBAT3L = 535,
    SPR_DBAT0U = 536,
    SPR_DBAT3L = 543,
    SPR_GQR0 = 912,
    SPR_GQR7 = 919,
    SPR_HID2 = 920,
    SPR_WPAR = 921,
    SPR_DMAU = 922,
    SPR_DMAL = 923,
    SPR_HID0 = 1008,
    SPR_HID1 = 1009,
    SPR_IABR = 1010,
    SPR_HID4 = 1011,
    SPR_DABR = 1013,
    SPR_L2CR = 1017,
    SPR_ICTC = 1019,
    SPR_THRM1 = 1020,
    SPR_THRM2 = 1021,
    SPR_THRM3 = 1022,
};

// Exception vector offsets inside the 0x80000000 vector page.
enum class Exception : u32 {
    SystemReset = 0x00100,
    MachineCheck = 0x00200,
    Dsi = 0x00300,
    Isi = 0x00400,
    ExternalInterrupt = 0x00500,
    Alignment = 0x00600,
    Program = 0x00700,
    FpUnavailable = 0x00800,
    Decrementer = 0x00900,
    SystemCall = 0x00C00,
    Trace = 0x00D00,
    PerformanceMonitor = 0x00F00,
};

union FpReg {
    u64 bits[2];  // [0] = ps0, [1] = ps1
};

enum class StopReason {
    Running,
    HostRequest,
    UnimplementedInstruction,
    TranslationFault,
    ExecutionFault,
    Breakpoint,
    HleExit,
};

struct CpuStop {
    StopReason reason = StopReason::Running;
    u32 address = 0;
    u32 instruction = 0;
    // The exception (its architected vector offset, e.g. 0x700) whose OS
    // entry was refused because the guest has not installed it - the stop
    // os_exception_entry.cpp makes; 0 for every other stop.
    u32 exception = 0;
    std::string detail;
};

class Cpu;

// Why recompiled code handed control back to the dispatcher. Every hand-back is
// a place the static recompilation is not yet complete, so these are the work
// list for finishing the native path - except Yield, which is the machine
// honouring a stop request or taking an interrupt and is expected.
enum class FallbackReason : u8 {
    None = 0,
    NotRecompiled,      // a call or jump whose target has no native code
    SystemCall,         // sc: the exception vector is not native yet
    ReturnFromInterrupt,// rfi
    Trap,               // tw / twi that trapped
    UnexpectedReturn,   // a callee returned somewhere other than the link address
    EndOfFunction,      // execution ran off the analysed extent of a function
    Yield,              // a stop request, or a deliverable interrupt at a recognition
                        // point (loop edge, mtmsr, after a call returned)
    VerifyCut,          // --verify-recomp only: a nested call handed back so the
                        // differential oracle can verify it as its own segment
    Count
};

const char* fallback_reason_name(FallbackReason reason) noexcept;

// Table of guest functions that have been statically recompiled to native
// x86-64 by tools/ppcrecomp. Lookup is a bitmap probe over the recompiled
// address span followed by a hash lookup, so the interpreter can ask "is there
// native code for this address" on every dispatch without paying for it.
class RecompiledCode {
public:
    using Fn = void (*)(Cpu&);

    struct Entry {
        u32 address;
        u32 size;
        Fn fn;
        const char* name;
    };

    // A translated function's address span, plus the entry point that can start
    // it at any instruction inside that span. The guest computes branch targets
    // - jump tables, function pointers aimed into the middle of a routine, an
    // SRR0 that is not a block boundary - and those addresses are not declared
    // entry points. Rather than interpret from there, the dispatcher finds the
    // owning function here and enters it at exactly that instruction.
    using AtFn = void (*)(Cpu&, u32);
    struct Range {
        u32 begin;
        u32 end;  // exclusive
        AtFn at;
    };

    void install(const Entry* entries, std::size_t count);
    // Ranges must be sorted by begin and non-overlapping.
    void install_ranges(const Range* ranges, std::size_t count) noexcept {
        ranges_ = ranges;
        range_count_ = count;
    }

    // Cold path: only consulted when lookup() found no declared entry.
    const Range* find_range(u32 address) const noexcept {
        std::size_t low = 0;
        std::size_t high = range_count_;
        while (low < high) {
            const std::size_t middle = low + (high - low) / 2u;
            if (address < ranges_[middle].begin) {
                high = middle;
            } else if (address >= ranges_[middle].end) {
                low = middle + 1u;
            } else {
                return &ranges_[middle];
            }
        }
        return nullptr;
    }

    // Every translated entry is an address of the program image: no guest code
    // runs in real mode (the OS exception entry is native, os_exception_entry.cpp,
    // and the BAT set-up that ran at a physical alias is owned), so there is no
    // real-mode table (docs/memory-architecture-20261006/README.md 2.2).
    Fn lookup(u32 address) const noexcept {
        if (address < low_ || address >= high_) {
            return nullptr;
        }
        // RAM-02: present_ deleted. install() set its bit and the slot together
        // in one statement pair, so the bit was exactly "this slot was
        // assigned" - and slots_ is all-nullptr before the fill, so the null
        // pointer already IS the absence marker. Identical even for a null fn:
        // the old code found the bit set and then returned that same nullptr.
        // RD1 (2026-10-20): a slot is the function's signed 32-bit distance
        // from kDispatchAnchor (cpu.cpp), 0 = absent: half the table of 8-byte
        // pointers. No translated function is the anchor, so 0 is never a real
        // distance; install() proves every entry round-trips.
        const s32 delta = slots_[(address - low_) >> 2];
        if (delta == 0) return nullptr;
        return reinterpret_cast<Fn>(anchor_ + static_cast<std::intptr_t>(delta));
    }

    std::size_t size() const noexcept { return count_; }
    u32 low() const noexcept { return low_; }
    u32 high() const noexcept { return high_; }
    const Entry* entries() const noexcept { return entries_; }
    // Committed lookup-table memory. The dense span is proportional to
    // high_ - low_, so a single stray entry far from the code region multiplies
    // this; install() warns loudly if it happens again.
    std::size_t table_bytes() const noexcept {
        return slots_.size() * sizeof(s32);
    }

private:
    const Entry* entries_ = nullptr;
    std::size_t count_ = 0;
    u32 low_ = 0;
    u32 high_ = 0;
    std::intptr_t anchor_ = 0;  // RD1: address of kDispatchAnchor
    std::vector<s32> slots_;    // RD1: distance from anchor_, 0 = no entry
    const Range* ranges_ = nullptr;
    std::size_t range_count_ = 0;
};

class Cpu {
public:
    explicit Cpu(GuestMemory& memory);
    ~Cpu();
    Cpu(const Cpu&) = delete;
    Cpu& operator=(const Cpu&) = delete;

    // ---- architected state -------------------------------------------------
    u32 gpr[32]{};
    FpReg fpr[32]{};
    u32 pc = 0;
    u32 npc = 0;
    u32 lr = 0;
    u32 ctr = 0;
    u32 xer = 0;
    u32 cr = 0;
    // MSR is private (C2, 2026-09-28): every write must go through set_msr(),
    // which re-selects the host page table for the data-translation mode.
    u32 msr() const noexcept { return msr_; }
    void set_msr(u32 value) noexcept {
        msr_ = value;
        const bool translated = (value & kHostPageMsrMask) == kHostPageMsrValue;
        // FM: the flat window while serving in translated supervisor mode, the
        // all-faulting window otherwise (the page table that made this choice
        // before is gone, FM step 2).
        fm_base_ = translated && serving_pages_ ? fm_window_ : fm_null_window_;
    }
    u32 fpscr = 0;
    std::array<u32, 1024> spr{};
    std::array<u32, 16> sr{};  // segment registers

    // ---- game time (docs/native-time-20261005/DESIGN.md) ---------------------
    //
    // There is no instruction clock. Guest code runs in zero game time; the
    // game clock advances only at a wait point (System::advance_game_time:
    // the game's frame limiter, the OS idle loop), event by event. The time
    // base and the decrementer are views of that clock: a time-base write moves
    // the view's offset, a decrementer write re-arms its sign-edge deadline
    // (GameEvent::Decrementer) on the clock. Both are exact at every boundary
    // because nothing between two wait points can move the clock.
    GameClock& game_clock() noexcept { return clock_; }
    const GameClock& game_clock() const noexcept { return clock_; }
    u64 time_base() const noexcept { return clock_.now() + time_base_offset_; }
    u32 decrementer() const noexcept {
        return dec_value_ - static_cast<u32>(clock_.now() - dec_written_at_);
    }
    // mttb / mtdec, savestate load and test setup.
    void set_time_base(u64 value) noexcept;
    void set_decrementer(u32 value) noexcept;
    // The decrementer's 0 -> 1 sign edge (GameEvent::Decrementer, delivered by
    // the scheduler at its deadline): latch the exception condition - whatever
    // MSR[EE] is; EE = 0 delays it - and re-arm the next edge 2^32 ticks on.
    void latch_decrementer() noexcept;
    // mtspr HID2, from translated code (ppc::write_spr) and the interpreter.
    // LSQE and PSE are machine invariants (kHid2PairedSingleEnables): a value
    // that would clear either is refused - the machine stops with a diagnostic
    // and HID2 keeps its value - because neither engine tests them.
    void write_hid2(u32 value);

    // ---- control -----------------------------------------------------------
    void reset();
    void set_entry(u32 address);

    // ---- development savestate (gekko/savestate.h) --------------------------
    // Appends / consumes the complete architected state plus the private FPR
    // exception context needed to resume an interrupted guest thread. The
    // architected prefix is unchanged through save version 4; version 5 adds
    // the private context after it; version 6 stores the lwarx reservation in
    // the architected block's former tail padding (same block size, ignored
    // and cleared for older versions); version 7 appends the host-private
    // timeline block (hardware-service phase and the latched data-access
    // fault), zeroed for older versions - which is exactly what a fresh
    // process loading them always had. Loading invalidates every memoised host
    // page, because the SPR block includes the BATs.
    void save_state(std::vector<u8>& out) const;
    bool load_state(const u8*& cursor, const u8* end, u32 save_version);

    // Runs the guest until the machine stops: a host stop request
    // (request_stop, served at the next recognition point), a fault, or a
    // place native execution cannot continue. Nothing else bounds a run.
    void run();
    // REFERENCE BUILD ONLY (defined with the interpreter): at most `max_steps`
    // dispatches, each one interpreted instruction or one native call tree.
    // The tests' bound for interpreter runs; production has no such bound.
    u64 run_steps(u64 max_steps);
    void step();

    // Host snapshot of all 32 FPRs + FPSCR across an exception, restored on the
    // matching rfi. Gekko does NOT preserve FPRs across an exception - FPU
    // ownership is the 0x800 handler's job - so this is host behaviour the
    // hardware does not have, and cpu.cpp already notes that discarding the
    // snapshot "is exactly what the hardware does".
    //
    // Default OFF as of 2026-09-05. Measured over the broadest route available
    // (cold boot -> menus -> Lava Shelter -> 60 s gameplay, 400e9 retired,
    // 1,922,305 exceptions): 1,141,282 restores performed, of which
    // **0** changed a single register. It fires constantly and does nothing.
    // Enable with --fpr-shadow to restore the old behaviour; the counters
    // fpr_restore_changed / _changed_registers say whether it would have
    // mattered on any route you try.
    void set_interrupt_fpr_shadow(bool enabled) noexcept { interrupt_fpr_shadow_ = enabled; }
    bool interrupt_fpr_shadow() const noexcept { return interrupt_fpr_shadow_; }

    // Stops the machine at the next translated back edge or call boundary
    // (needs_dispatch() and stopped(), respectively). Must be called on the
    // thread executing guest code: it writes the stop flag that thread polls.
    void request_stop(StopReason reason, const std::string& detail);
    const CpuStop& stop() const noexcept { return stop_; }
    bool stopped() const noexcept { return stop_.reason != StopReason::Running; }
    // A stop was requested and the running dispatch has not ended yet.
    bool stop_requested() const noexcept { return stop_requested_.load(std::memory_order_relaxed); }
    // Discards the stop REPORT. A latched data-access fault (below) is machine
    // state, not a report: run() re-raises its stop until reset() or
    // load_state() replaces the machine.
    void clear_stop() noexcept { stop_ = CpuStop{}; }
    // A stop requested from a host thread other than the one executing guest
    // code (System's wait-point watchdog). The only cross-thread entry into
    // the core: it raises the flag translated back edges poll, and the
    // dispatcher turns the request into an ordinary stop on the guest thread,
    // its detail followed by the guest location where the guest handed back.
    void request_stop_from_host(StopReason reason, std::string detail);
    // pc, LR, CTR, r1 and the saved LRs along the guest back chain. Diagnostic
    // peeks only (read_guest_u32): touches nothing.
    std::string guest_location(unsigned max_frames = 12);

    // ---- data-access fault (audit memory-model-3) ----------------------------
    // With MSR[DR]=1 and no DBAT match the hardware takes a DSI (0x300) on the
    // faulting instruction. That exception cannot be raised precisely here:
    // translated code does not publish pc per instruction, so the faulting
    // instruction's SRR0 does not exist at the access. The core instead HALTS
    // the machine at
    // the first fault and keeps that fault's description: DAR, DSISR and the
    // latched record are written once and never overwritten; every later guest
    // memory access in the unwinding native code is suppressed (loads read 0,
    // stores, FIFO stores and MMIO access are dropped); every later call
    // unwinds without running its callee. See Cpu::memory_fault.
    struct MemoryFault {
        bool latched = false;
        bool write = false;
        // The window had nothing at the address (an unmapped access with
        // unmapped-access stops on); otherwise data translation was off.
        bool unmapped = false;
        u32 effective = 0;
        // Guest pc when the fault was taken: exact for the interpreter, the
        // last guest address native code published for translated code.
        u32 pc = 0;
    };
    const MemoryFault& memory_fault() const noexcept { return memory_fault_; }

    // ---- asynchronous interrupts (audit batch b5) ----------------------------
    //
    // interrupt_lines_ holds every maskable asynchronous exception CONDITION:
    //   kInterruptExternal    - the INT line from the interrupt controller
    //                           (level: follows set_external_interrupt);
    //   kInterruptDecrementer - DEC bit 0 went 0 -> 1 (gekko_user_manual.txt
    //                           PAGE 159 Table 4-2). Latched on EVERY such edge,
    //                           whatever MSR[EE] is, and cleared only when 0x900
    //                           is taken: EE = 0 "delays recognition of external
    //                           interrupts and decrementer exception conditions"
    //                           (PAGE 164 Table 4-4), it does not discard them.
    // A condition is DELIVERABLE while MSR[EE] = 1. Both engines take a
    // deliverable condition only at these instruction boundaries (the
    // recognition points), external before decrementer (PAGE 162 Table 4-3):
    //   R1 every Cpu::run dispatch: before native code is entered and before
    //      each interpreted instruction;
    //   R2 after a taken bclr (any BO, LK 0 or 1) and after an HLE-served call:
    //      native returns to the caller's `if (c.exiting()) return;` or to run();
    //   R3 after a taken direct b/bc with LK = 0 whose target <= its own
    //      address, inside or outside the function's record (needs_dispatch);
    //   R4 immediately before and immediately after mtmsr (interrupt_pending;
    //      PAGE 475: an EE 0 -> 1 write takes it "before the next instruction");
    //   R5 after rfi (both engines hand to R1).
    // sc, a taken trap and an FP-unavailable fault are not recognition points:
    // the instruction's own exception is taken and the condition stays pending
    // (PAGE 161), as the manual orders it.
    //
    // No predicate below gained a term for this. A condition that becomes
    // deliverable while native code runs (set_external_interrupt with EE = 1;
    // the decrementer latch happens only at a wait point) also sets
    // kExitInterrupt in exit_pending_, which the call/return check exiting()
    // already tests; back edges and mtmsr test the same one-byte word they
    // always tested. The only other way a condition becomes deliverable inside
    // native code is an EE 0 -> 1 write, and only mtmsr (R4) and rfi (R5) write
    // EE there - both check or hand back immediately. exit_pending_ is only
    // meaningful inside native frames: every native entry re-arms it.
    static constexpr u8 kInterruptExternal = 0x01u;
    static constexpr u8 kInterruptDecrementer = 0x02u;
    // A native interrupt source (gekko/native_interrupts.h) is pending and
    // unmasked: level, like the external line. Taken after external and
    // before the decrementer, through the installed native handler instead of
    // an exception vector (the handler enters the OS's own context protocol).
    static constexpr u8 kInterruptNative = 0x04u;

    // Asynchronous external interrupt line, driven by the interrupt controller.
    // Must be called on the thread executing guest code (it writes the exit
    // request that thread's native frames poll).
    void set_external_interrupt(bool asserted) noexcept {
        if (!asserted) {
            interrupt_lines_ = static_cast<u8>(interrupt_lines_ & ~kInterruptExternal);
            return;
        }
        interrupt_lines_ = static_cast<u8>(interrupt_lines_ | kInterruptExternal);
        if ((msr_ & kMsrEE) != 0u) exit_pending_ = static_cast<u8>(exit_pending_ | kExitInterrupt);
    }
    bool external_interrupt() const noexcept {
        return (interrupt_lines_ & kInterruptExternal) != 0u;
    }
    // The native interrupt line (kInterruptNative), driven by NativeInterrupts.
    // Same contract as set_external_interrupt.
    void set_native_interrupt(bool asserted) noexcept {
        if (!asserted) {
            interrupt_lines_ = static_cast<u8>(interrupt_lines_ & ~kInterruptNative);
            return;
        }
        interrupt_lines_ = static_cast<u8>(interrupt_lines_ | kInterruptNative);
        if ((msr_ & kMsrEE) != 0u) exit_pending_ = static_cast<u8>(exit_pending_ | kExitInterrupt);
    }
    bool native_interrupt() const noexcept {
        return (interrupt_lines_ & kInterruptNative) != 0u;
    }
    // Takes the native line at a recognition point (the dispatcher's R1, the
    // interpreter's step): the handler starts the delivery - it writes the
    // guest state, sets pc/npc to the first guest instruction to run - or,
    // when the source went away, lowers the line and leaves pc alone.
    using NativeInterruptHandler = std::function<void(Cpu&)>;
    void set_native_interrupt_handler(NativeInterruptHandler handler) {
        native_interrupt_handler_ = std::move(handler);
    }
    // When the external line and the native line are both pending, the OS
    // takes whichever its priority table ranks first (__OSDispatchInterrupt,
    // the rows at 0x805625C8). NativeInterrupts installs the comparison: true
    // means the native source goes first. Without one the external line does.
    using NativeInterruptFirst = std::function<bool(Cpu&)>;
    void set_native_interrupt_first(NativeInterruptFirst first) {
        native_interrupt_first_ = std::move(first);
    }

    // ---- platform sites (extract/platform-sites.txt) ------------------------
    // A listed guest access to a platform register made by code whose own
    // subsystem is not native yet (docs/native-time-20261005/STAGE2_VI.md,
    // D5): the generator emits these calls in place of the bus access, and
    // the installed native subsystem answers - the platform's fact for a
    // read, the native effect of a write. `site` is the instruction address,
    // `effective` the address it would have accessed; the handler refuses
    // any other register (fail closed). Nothing else answers these addresses.
    using PlatformRead = std::function<u16(Cpu&, u32 site, u32 effective)>;
    using PlatformWrite = std::function<void(Cpu&, u32 site, u32 effective, u16 value)>;
    void set_platform_sites(PlatformRead read, PlatformWrite write) {
        platform_read_ = std::move(read);
        platform_write_ = std::move(write);
    }
    u16 platform_read16(u32 site, u32 effective);
    void platform_write16(u32 site, u32 effective, u16 value);
    // A listed li of a console constant (the length of a console video field,
    // ...): the native platform's value for that site, or a refusal.
    using PlatformValue = std::function<u32(Cpu&, u32 site)>;
    void set_platform_values(PlatformValue value) { platform_value_ = std::move(value); }
    u32 platform_value(u32 site);
    // The generated table of listed sites (recomp::kPlatformSites, sorted by
    // address; gekko/platform_sites.h): the interpreter applies it to the same
    // words the generator lowered (Cpu::execute). System installs it.
    void set_platform_site_table(const PlatformSiteRecord* sites, std::size_t count) noexcept {
        platform_site_table_ = sites;
        platform_site_table_count_ = count;
    }
    const PlatformSiteRecord* find_platform_site(u32 address) const noexcept;
    // A decrementer exception condition is pending (taken when EE allows).
    bool decrementer_pending() const noexcept {
        return (interrupt_lines_ & kInterruptDecrementer) != 0u;
    }
    // True when an asynchronous exception is deliverable: the machine takes it
    // at the next recognition point. Recompiled code tests this around mtmsr.
    bool interrupt_pending() const noexcept {
        return interrupt_lines_ != 0u && (msr_ & kMsrEE) != 0u;
    }

    // Recompiled code runs a whole call tree without returning to the dispatcher,
    // so a guest loop would otherwise be unstoppable and would never see an
    // interrupt. Every loop back edge (R3 above) asks this once per iteration: it
    // is true when an interrupt is deliverable or a stop was requested, and the
    // loop then hands control back with the guest state consistent.
    bool needs_dispatch() const noexcept {
        return stop_requested_.load(std::memory_order_relaxed) ||
               (interrupt_lines_ != 0u && (msr_ & kMsrEE) != 0u);
    }

    // The exception vector does not save FPRs. Handlers that use float as
    // scratch would otherwise resume the interrupted code with f1-f4 trashed
    // (Westopolis particle wrap at 0x804349F8). The first raise snapshots;
    // rfi restores unless a nested raise already owns the snapshot.
    void restore_interrupt_fpr() noexcept;

    // Record where an exception will actually resume, when the raiser rewrites
    // SRR0 after raise_exception (sc resumes at the instruction AFTER the sc).
    // restore_interrupt_fpr compares against this to decide whether the rfi is
    // returning to the context it interrupted.
    void note_exception_resume(u32 srr0) noexcept;

    u64 interrupt_fpr_context_switches() const noexcept {
        return interrupt_fpr_context_switches_;
    }

    // A wait point. The generator emits this call on the taken back edge of
    // every side-effect-free polling loop (extract/wait-points.txt, from
    // tools/ppcrecomp/wait_points.py): SelectThread's idle spin on its ready
    // mask, RenderWare's wait for its XFB ring, ARQ completion flags, device
    // busy bits, time-base delays, halts. Such a loop's iteration is a pure
    // function of what it reads, so nothing the guest does can end it; only an
    // event can. The installed idle handler advances the game clock to its
    // next event and delivers it (System::idle_wait); the loop then re-reads
    // what it polls, and its needs_dispatch() test takes an interrupt the
    // event made deliverable. Without a handler (tests) nothing can change, so
    // the machine stops.
    using IdleHandler = std::function<void(Cpu&)>;
    void set_idle_handler(IdleHandler handler) { idle_handler_ = std::move(handler); }
    void idle_wait();
    // A `progress` back edge (extract/wait-points.txt): a loop whose iteration
    // calls or stores, so only the run time can tell whether it waits. Taken,
    // the back edge compares the registers with this site's previous visit;
    // unchanged, the next iteration runs under a GuestMemory watch; when that
    // iteration again leaves the registers unchanged and made no net change
    // to memory, it was a fixed point - the next one would be identical, so
    // only an event can end the loop - and the clock advances to its next
    // event (as idle_wait). A watch ends at every dispatch; tracking ends at
    // every wait and when another site polls.
    void poll_point(u32 site);
    void end_poll_tracking() noexcept;

    // An exception: the architected entry (SRR0, SRR1, MSR, the interrupt FPR
    // bookkeeping), then the OS's vector work done natively - the OSContext
    // save and the entry into the handler __OSExceptionTable names, with
    // translation on (os_exception_entry.cpp). npc is the handler. No guest
    // code runs in real mode.
    void raise_exception(Exception vector, u32 srr1_bits = 0u);
    // sc at `address` (resumes after it).
    void system_call(u32 address);
    // The architected half of an exception alone (SRR0, SRR1, the exception
    // MSR, the interrupt FPR bookkeeping), without the OS entry: raise_exception
    // is this followed by enter_os_exception. Public for the FPR-shadow tests.
    void exception_entry(Exception vector);
    // The processor half of an exception entry, for a native interrupt
    // delivery (gekko/native_interrupts.h): SRR0 = pc, SRR1, the exception
    // MSR with translation on (what the OS vector's rfi into its handler
    // leaves), and the interrupt-FPR bookkeeping its closing rfi pairs with.
    // pc is not changed; no vector is fetched.
    void enter_native_interrupt() noexcept;

    // ---- address translation ----------------------------------------------
    // The game's addresses are GuestMemory's flat 32-bit window
    // (docs/memory-architecture-20261006/README.md): an address is itself, and
    // the page table says whether anything is there. Translation is only the
    // guest's MSR[IR]/MSR[DR] being on - code that turned it off would be
    // using GameCube physical addresses, which do not exist here, so such an
    // access is refused (false: the access takes a data fault). The guest
    // BATs are plain SPR storage the guest may read back: they decide nothing.
    bool translate(u32 effective, bool is_write, bool is_instruction, u32& address);
    // Host-side diagnostic peeks (audit memory-model-4). They read like a
    // guest load but touch nothing: no DAR, no stop, no fault latch and no
    // fixture or unmapped access. Only mapped guest memory is readable; an
    // unmapped byte reads as 0 (read_guest_u32/u8) or ends the string.
    u32 read_guest_u32(u32 effective);
    u8 read_guest_u8(u32 effective);
    std::string read_guest_string(u32 effective, u32 limit = 256);
    // False (and value untouched) when the byte is not host-backed RAM.
    bool peek_guest_u8(u32 effective, u8& value);
    // Architected store, identical to a guest stw (not a diagnostic).
    void write_guest_u32(u32 effective, u32 value);

    // ---- the page table ------------------------------------------------------
    //
    // The inline accessors read GuestMemory's page table directly: one host
    // pointer per 16 KiB page of the window, nullptr where nothing is
    // committed. GuestMemory writes it when a region is mapped, committed or
    // unmapped; the CPU keeps no copy.
    static constexpr u32 kHostPageShift = GuestMemory::kPageShift;
    static constexpr u32 kHostPageSize = GuestMemory::kPageSize;
    static constexpr u32 kHostPageOffsetMask = GuestMemory::kPageOffsetMask;
    static constexpr std::size_t kHostPageCount = GuestMemory::kPageCount;

    // Accesses are served from the page table only in supervisor mode with
    // data translation on, and only while nothing must see every access (the
    // oracle's journal, the poll watch, an access trace) and no data fault is
    // latched. Otherwise every access takes the slow path.
    static constexpr u32 kHostPageMsrMask = kMsrDR | kMsrPR;
    static constexpr u32 kHostPageMsrValue = kMsrDR;

    // Re-decides whether the page table serves accesses (journalling, the
    // access trace and the fault latch changed): reset, the fault latch,
    // journal / watch begin and end, trace changes, savestate load.
    void select_memory_window() noexcept;

    // Guest memory at a game address, exactly as an instruction reaches it.
    // instruction would. Statically recompiled code goes through these so its
    // data path is the same one the interpreter uses.
    // CPU-22: the fast half has no call on any edge, so these are leaves again
    // and the hit path stops paying a prologue and a shadow frame. Everything
    // the fast half declines is decided by *_cold, which is the old body.
    // GEKKO_NOINLINE is deliberate: the bodies are now small enough that MSVC
    // could start inlining them at ~433k generated sites, which is the exact
    // image growth CPU-04, CPU-13 and CPU-21 each measured as a loss.
    // FM (2026-10-20): the flat native memory map. A guest access is ONE host
    // access at fm_base_ + effective (GuestMemory::fm_window(): every region's
    // pages at their guest address, everything else faulting) and a byte swap.
    // Volatile keeps it exactly one MOV / MOVZX / MOVBE in program order: no
    // folding into an ALU operand, no merging or dropping of accesses, the
    // same sequence of accesses the NOINLINE calls made. Every access the old
    // fast half declined faults here instead, and Cpu::fm_fault (the vectored
    // handler, cpu.cpp) decides it through the same *_slow body that decided
    // it before: unmapped / uncommitted / page-table-off (MSR[DR] = 0 or user
    // mode, fm_base_ = the null window) / journal / access trace / latched
    // fault. Inline everywhere: the access is no longer than the call it
    // replaces, so the image growth of CPU-04/13/21 does not recur.
    GEKKO_ALWAYS_INLINE u8 mem_read8(u32 effective) { return fm_load8(fm_base_ + effective); }
    GEKKO_ALWAYS_INLINE u16 mem_read16(u32 effective) { return fm_swap16(fm_load16(fm_base_ + effective)); }
    GEKKO_ALWAYS_INLINE u32 mem_read32(u32 effective) { return fm_swap32(fm_load32(fm_base_ + effective)); }
    GEKKO_ALWAYS_INLINE u64 mem_read64(u32 effective) { return fm_swap64(fm_load64(fm_base_ + effective)); }
    GEKKO_ALWAYS_INLINE void mem_write8(u32 effective, u8 value) { fm_store8(fm_base_ + effective, value); }
    GEKKO_ALWAYS_INLINE void mem_write16(u32 effective, u16 value) {
        fm_store16(fm_base_ + effective, fm_swap16(value));
    }
    GEKKO_ALWAYS_INLINE void mem_write32(u32 effective, u32 value) {
        fm_store32(fm_base_ + effective, fm_swap32(value));
    }
    GEKKO_ALWAYS_INLINE void mem_write64(u32 effective, u64 value) {
        fm_store64(fm_base_ + effective, fm_swap64(value));
    }
    // FM: the vectored-exception entry (cpu.cpp). Public so the process-wide
    // handler can reach the machine that owns a faulting window address.
    static long fm_fault(void* exception_pointers) noexcept;
    // FM: accesses the window did not serve and the handler resolved (all
    // machines of the process; exit report and the fault-injection tests).
    static u64 fm_fault_count() noexcept;
    // FM: this machine's flat window (tests address it directly).
    u8* fm_window() const noexcept { return fm_window_; }

    GuestMemory& guest_memory() noexcept { return memory_; }
    const GuestMemory& guest_memory() const noexcept { return memory_; }

    // ---- diagnostics -------------------------------------------------------
    struct TraceEntry {
        u32 address;
        u32 instruction;
    };
    void set_history_depth(std::size_t depth);
    std::vector<TraceEntry> history() const;
    void set_instruction_trace(bool enabled) noexcept { instruction_trace_ = enabled; }
    bool instruction_trace() const noexcept { return instruction_trace_; }

    // Host callbacks bound to guest addresses. A handler returning true means it
    // fully emulated the call and the interpreter must return to LR.
    using HleHandler = std::function<bool(Cpu&)>;
    void install_hle(u32 address, HleHandler handler, std::string name);
    // 2026-09-05 audit: every installed hook with its hit count.
    const auto& hle_entries() const noexcept { return hle_; }
    bool has_hle(u32 address) const;
    const std::string* hle_name(u32 address) const;
    // Generated entry thunks call this. A handler returning true has fully
    // served the call; r3 (and any other results) are already in guest state
    // and execution resumes at LR. Returning false runs the native body.
    //
    // The miss path is inline and call-free for ANY number of hooks (audit
    // dispatch-exceptions-3): one load of a direct-mapped slot keyed by the
    // address and two compares. A slot holds the one hooked address that maps
    // to it, kHleSlotEmpty (never equal to a word-aligned address), or
    // kHleSlotAll, which sends every address of that slot to try_hle_slow.
    // rebuild_hle_slots fills EVERY slot with kHleSlotAll while
    // --verify-recomp is on or a data-access fault is latched, so neither
    // condition costs the thunk a separate test. try_hle_slow stays exact.
    // The generated entry of a native owner (recomp::kNativeOwners): the
    // original body is not in the image, so the installed owner must serve the
    // call. A missing or declining owner stops the machine.
    void serve_native_owner(u32 address);
    bool try_hle(u32 address) {
        const u32 slot = hle_slots_[(address >> 2) & kHleSlotMask];
        if (slot != address && slot != kHleSlotAll) {
            return false;
        }
        return try_hle_slow(address);
    }
    // Calls that got past the inline filter. Counted on the out-of-line side
    // only, so the reject path stays call- and write-free.
    u64 hle_slow_calls() const noexcept { return hle_slow_calls_; }

    // ---- lowered call sites (gekko/lift_sites.h, extract/lift-sites.txt) ------
    //
    // The generated recomp::kLiftSites, sorted. Translated code runs a site's
    // lowering inline; the dispatcher and step() run the same function when
    // they arrive at the site, and continue after it.
    void set_lift_sites(const LiftSiteRecord* sites, std::size_t count) noexcept {
        lift_sites_ = sites;
        lift_site_count_ = count;
    }
    const LiftSiteRecord* find_lift_site(u32 address) const noexcept;
    std::size_t lift_site_count() const noexcept { return lift_site_count_; }
    // Emitted before every lowered site. Under --verify-recomp a pass ends at
    // the site, exactly as it ends before an owner call (try_hle_slow), and the
    // dispatcher runs the site once outside the oracle; while a data-access
    // fault unwinds, the site does not run. Otherwise false: the site runs.
    bool lift_site_cut(u32 site) {
        if (!verifying_ && !memory_fault_.latched) return false;
        return lift_site_cut_slow(site);
    }
    // The lifted subsystems the lowerings call (gekko/lifted.h). System sets it.
    void set_lifted(Lifted* lifted) noexcept { lifted_ = lifted; }
    Lifted& lifted() noexcept { return *lifted_; }
    // Sites the dispatcher or the interpreter ran (the cut ones and any
    // resumption exactly at a site).
    u64 lift_sites_dispatched() const noexcept { return lift_sites_dispatched_; }

    // ---- GX pipe sites (gekko/gx_sites.h, extract/gx-sites.txt) -------------
    //
    // The generator lowered every store of the image to the write-gather pipe;
    // the live ones call these (site = the store's address). The table is the
    // generated recomp::kGxSites: the interpreter applies it to the same words
    // (Cpu::execute), and with a sink installed a store reaching the pipe page
    // that the table does not hold stops the machine. Without a sink (unit
    // fixtures) the page is ordinary bus space.
    void set_gx_sites(const GxSiteRecord* sites, std::size_t count, GxSiteSink* sink) noexcept {
        gx_sites_ = sites;
        gx_site_count_ = count;
        gx_sink_ = sink;
    }
    const GxSiteRecord* find_gx_site(u32 address) const noexcept;
    void gx_draw_sync_token(u32 site, u16 token, bool interrupt);
    void gx_draw_done(u32 site, u32 value);
    void gx_copy_trigger(u32 site, u32 value);
    void gx_primitive_opcode(u32 site, u8 opcode);
    void gx_primitive_count(u32 site, u16 count);
    void gx_vertex(u32 site, u32 value, u32 bytes);
    // GXD: a GX pipe site the lifted build proved dead was reached: stop the
    // machine naming it (fail closed; the generator emits this instead of the
    // family's call, keeping the site's address computation and base update).
    void gx_dead_site(u32 site);
    u64 unlisted_gx_stores() const noexcept { return unlisted_gx_stores_; }

    // ---- no device registers (stage 6) ---------------------------------------
    //
    // Every GameCube device is native (docs/native-time-20261005/STAGE6_PI_MI.md,
    // D1): there is no register a guest access could reach. With this set - the
    // production System sets it - a guest load or store whose address
    // is neither guest memory nor a registered test fixture stops the machine,
    // naming the retired device page, what replaced it, the access and the guest
    // location (fail closed: a load yields 0 and the store lands nowhere until
    // the stop is taken). Unset (unit fixtures) such an access is
    // GuestMemory's counted unmapped access.
    void set_unmapped_access_stops(bool stops) noexcept { unmapped_access_stops_ = stops; }
    bool unmapped_access_stops() const noexcept { return unmapped_access_stops_; }
    u64 unmapped_accesses_stopped() const noexcept { return unmapped_accesses_stopped_; }

    // ---- statically recompiled code ----------------------------------------
    //
    // Recompiled functions run on this same Cpu object: they read and write the
    // architected state above and go through mem_read*/mem_write*, so the guest
    // cannot tell which engine executed a given function. Control returns to the
    // interpreter either normally (the function reached blr and left pc at the
    // link register) or by leave_to_dispatcher, which unwinds every native frame
    // with the guest state fully committed.
    void attach_recompiled(RecompiledCode* code) noexcept {
        pending_native_call_.reset();
        recompiled_ = code;
    }
    const RecompiledCode* recompiled() const noexcept { return recompiled_; }

    // Hands control back to the interpreter at the given guest address. Every
    // recompiled frame on the native stack returns immediately without executing
    // any more guest instructions; the guest is left in a fully consistent state
    // whose next instruction is resume_pc.
    void leave_to_dispatcher(u32 resume_pc) noexcept {
        leave_to_dispatcher(resume_pc, FallbackReason::NotRecompiled, 0);
    }

    void leave_to_dispatcher(u32 resume_pc, FallbackReason reason, u32 site) noexcept {
        pending_native_call_.reset();
        pc = resume_pc;
        npc = resume_pc;
        // A leave supersedes a published transfer: the dispatcher resumes at
        // resume_pc (R12; a body never publishes and then leaves, this only
        // states the precedence).
        exit_pending_ = static_cast<u8>((exit_pending_ & ~kExitTransfer) | kExitLeave);
        // Where it gave up: the dispatcher's no-progress test (Cpu::run).
        leave_site_ = site;
        leave_reason_ = reason;
        ++interpreter_exits;
        ++fallback_totals_[static_cast<std::size_t>(reason)];
        if (record_fallbacks_) {
            record_fallback(resume_pc, reason, site);
        }
    }

    void handoff_native_call(u32 target, u32 site) noexcept;

    // ---- interpreter-fallback audit ---------------------------------------
    struct FallbackSite {
        u32 target = 0;      // guest address the interpreter had to take over at
        u32 caller = 0;      // guest address of the instruction that gave up
        FallbackReason reason = FallbackReason::None;
        u64 count = 0;
        u64 first_dispatch = 0;  // dispatch sequence when it first happened (boot order)
    };
    // One uninterrupted run of interpreted instructions, attributed to the guest
    // address the dispatcher could not find native code for. This is always on:
    // it costs nothing on the native path, because it is only touched when the
    // machine is about to interpret an instruction anyway, and it is the only
    // measurement that says which missing translation is expensive rather than
    // merely frequent.
    struct InterpreterStrand {
        u32 address = 0;            // guest PC the strand started at
        u32 link = 0;               // LR on entry: who was calling
        u32 last_link = 0;
        u64 entries = 0;            // how many times execution stranded here
        u64 instructions = 0;       // guest instructions interpreted in total
        u64 longest_instructions = 0;
        u64 host_ns = 0;            // host time spent interpreting them
        u64 first_dispatch = 0;     // dispatch sequence when it first happened
    };
    std::vector<InterpreterStrand> interpreter_strands() const;
    u64 interpreted_instructions() const noexcept { return interpreted_instructions_; }
    u64 interpreted_ns() const noexcept { return interpreted_ns_; }

    void set_record_fallbacks(bool enabled) noexcept { record_fallbacks_ = enabled; }
    std::vector<FallbackSite> fallback_sites() const;
    u64 fallback_total(FallbackReason reason) const noexcept {
        return fallback_totals_[static_cast<std::size_t>(reason)];
    }
    // Tested after every native call. True when a callee handed back
    // (kExitLeave), when an interrupt became deliverable inside the call
    // (kExitInterrupt: pc is then the link address or wherever the callee
    // returned to, a precise R2 recognition point), when the callee published
    // a transfer (kExitTransfer, R12), or when the machine stopped.
    bool exiting() const noexcept { return exit_pending_ != 0u || stopped(); }

    // ---- LK=0 transfers (audit R12, 2026-09-28) ----------------------------
    //
    // A non-linking transfer to another translated record - `b`/`bc` out of
    // the record, the end-of-record fall-through, `bcctr` (jump_indirect), and
    // the hand-over of a callee that returned somewhere other than its link
    // (resume_elsewhere) - is NIA <- target and nothing else: no link, no
    // return address, nothing on the guest stack. It used to be emitted as a
    // host CALL in tail position (`fn_target(c); return;`), so whether it kept
    // a host frame was MSVC's tail-call decision, and a guest cycle of such
    // transfers grew the host stack by one frame per lap whenever MSVC chose
    // `call` (the f1c `_b` shapes, fn_803A1AAC/fn_803A1AD4; C1 changed which
    // functions MSVC tail-called).
    //
    // Now the issuing body PUBLISHES the target's entry (transfer_to) and
    // returns, releasing its frame before the target is entered. The frame
    // that is waiting for a return - the generated code after a genuine
    // linked call, or the dispatcher - runs the published entries one after
    // another (finish_transfers), so a chain or cycle of any length costs one
    // body frame at a time. Host frames are bounded by the guest's own
    // linked-call nesting. No recognition point is added: the target is
    // entered with every other exit_pending_ bit untouched and without a
    // budget, stop or interrupt test, exactly as the tail call entered it; its
    // own R2/R3/R4 points decide, as before. c.pc is not written either (the
    // tail call did not write it; the thunk's try_hle uses its own address).
    void transfer_to(RecompiledCode::Fn entry) noexcept {
        transfer_fn_ = entry;
        exit_pending_ = static_cast<u8>(exit_pending_ | kExitTransfer);
    }
    // Runs every published transfer, then answers exiting() for the caller's
    // R2 check. Generated code calls it only when exiting() was already true,
    // so the normal return path is unchanged: `if (c.exiting() &&
    // c.finish_transfers()) return;`. Host code that enters translated code
    // (Cpu::run, the reference verifier) calls it after the entry returns.
    GEKKO_NOINLINE bool finish_transfers();

    u64 recompiled_calls = 0;
    u64 interpreter_exits = 0;
    // R12: the entry the last transfer_to published (valid while
    // exit_pending_ has kExitTransfer). Occupies the 8 bytes the deleted
    // trampoline latch used, so no later field moved.
    RecompiledCode::Fn transfer_fn_ = nullptr;
    // recomp::jump_indirect / recomp::resume_elsewhere: transfers whose target
    // had native code (published, R12; before R12 each was a host call that
    // could keep a frame) versus hand-backs to the dispatcher. Same events and
    // counts as before R12.
    u64 native_transfer_calls = 0;
    u64 native_transfer_leaves = 0;
    // R12: every published transfer finish_transfers ran, including the
    // direct `b` / fall-through transfers the two counters above never saw.
    u64 native_transfers_driven = 0;

    // Why the dispatcher did not put this instruction into native code.
    u64 dispatch_blocked_interrupt = 0;
    u64 dispatch_blocked_hle = 0;
    u64 dispatch_no_native_code = 0;
    // Dispatches that entered a translated function at an instruction the guest
    // computed rather than at a declared entry point. Native, not interpreted.
    u64 dispatch_entered_mid_function = 0;

    // ---- differential verification ----------------------------------------
    //
    // Runs every dispatched recompiled function twice: once natively, then -
    // with the guest state and every memory write rolled back - through the
    // interpreter, and compares the whole architected state and the bytes
    // written. There is no instruction count, so the replay ends at the point
    // the native pass ended, recognised three ways: the segment's function
    // returned (the taken bclr that lands where the native pass returned,
    // same pc and r1 - a callee entered through an interior thunk is not cut,
    // and its own return lands after its call site), it raised its own
    // exception (as many exceptions, the same vector), or it stopped on the
    // per-call cut at its first nested callee (that callee's entry with the
    // same LR and r1). A segment that ended any other way, reached a hardware
    // register or a wait point is skipped rather than repeated: a device side
    // effect or a delivered event cannot be undone.
    struct VerifyReport {
        u64 verified = 0;
        u64 skipped_device = 0;
        u64 skipped_exit = 0;
        u64 mismatches = 0;
        // Guest calls an HLE handler fully served. The handler runs exactly
        // once and outside the oracle: it is host code, not translated code,
        // so there is nothing for the two engines to compare.
        u64 skipped_hle = 0;
        // Interpreted instructions the replays compared (the replay's own
        // count; native code has none). The segment counts alone once hid a
        // collapse of the oracle behind plausible-looking totals.
        u64 verified_instructions = 0;
        u64 native_journal_peak_entries = 0;
        u64 interpreter_journal_peak_entries = 0;
        u64 unique_native_bytes_peak = 0;
        u64 journal_scratch_peak_bytes = 0;
        std::vector<std::string> messages;
        // Mismatches get their own reserve: the shared message window once
        // filled with sixteen routine device skips while the run's single
        // mismatch - the only line that mattered - was crowded out unseen.
        std::vector<std::string> mismatch_messages;
    };
    // max_calls bounds the verification cost of a long route: once that many
    // segments have been checked the rest of the run executes normally (still
    // through per-call dispatch cuts, so control flow stays uniform). With
    // per-call granularity a 400M-instruction route produces roughly 100-500
    // thousand segments, so the default leaves headroom rather than truncating
    // the standard oracle budget.
    void set_verify_recompiled(bool enabled, std::size_t max_messages = 8,
                               u64 max_calls = 2000000);
    const VerifyReport& verify_report() const noexcept { return verify_; }

    // ---- helpers used by devices and the loader ----------------------------
    void set_cr_field(u32 field, u32 value);
    u32 cr_field(u32 field) const;

private:
    void execute(u32 instruction);
    void unimplemented(u32 instruction, const char* form);
    // Takes the highest-priority deliverable condition (external, then
    // decrementer). Caller guarantees interrupt_pending(); pc becomes the vector.
    void take_pending_interrupt();
    // The byte value exit_pending_ must start a native entry with: an interrupt
    // that is already deliverable (a handler the dispatcher ran after its
    // recognition check retired into an edge) is still requested.
    u8 native_entry_exit_state() const noexcept {
        return interrupt_pending() ? kExitInterrupt : u8{0};
    }
    void record_fallback(u32 target, FallbackReason reason, u32 site);
    void open_interpreter_strand();
    void close_interpreter_strand();

    struct ArchitectedState;
    struct InterruptFprState;
    struct TimelineState;
    void capture_state(ArchitectedState& state) const;
    void restore_state(const ArchitectedState& state);
    void capture_interrupt_fpr_state(InterruptFprState& state) const noexcept;
    void restore_interrupt_fpr_state(const InterruptFprState& state) noexcept;
    void verify_dispatch(RecompiledCode::Fn native, u32 entry);

    void op_group4(u32 instruction);   // paired singles
    void op_group19(u32 instruction);  // branch to LR/CTR, CR logic
    void op_group31(u32 instruction);  // integer and extended
    void op_group59(u32 instruction);  // single-precision FP
    void op_group63(u32 instruction);  // double-precision FP
    void op_psq_load(u32 instruction, bool indexed, bool update);
    void op_psq_store(u32 instruction, bool indexed, bool update);

    void update_cr0(u32 value);
    void update_cr1();
    void set_xer_carry(bool carry);
    void set_xer_overflow(bool overflow);

    float quantise_load(u32 address, u32 type, u32 scale);
    void quantise_store(u32 address, float value, u32 type, u32 scale);

    // Mapped guest memory for a slow-path access, or nullptr (a fixture or
    // nothing at all); reports the access trace.
    u8* memory_access(u32 address, u32 size, bool writing);

    u8 mem_read8_slow(u32 effective);
    u16 mem_read16_slow(u32 effective);
    u32 mem_read32_slow(u32 effective);
    u64 mem_read64_slow(u32 effective);
    void mem_write8_slow(u32 effective, u8 value);
    void mem_write16_slow(u32 effective, u16 value);
    void mem_write32_slow(u32 effective, u32 value);
    void mem_write64_slow(u32 effective, u64 value);
    // A lowered GX site was reached: notes the host effect; false when the
    // call must not happen (a latched data fault, or no sink: stops).
    bool gx_site_enter(u32 site);
    // The interpreter's side of a listed GX site (Cpu::execute).
    void execute_gx_site(const GxSiteRecord& site, u32 instruction);
    // The interpreter's side of a listed platform site (Cpu::execute).
    void execute_platform_site(const PlatformSiteRecord& site, u32 instruction);
    // A store reached the pipe page that no listed site made: stops.
    void unlisted_gx_store(u32 address, u64 value, u32 bytes);
    // With set_unmapped_access_stops: true when a guest access at `address`
    // has nothing to serve it (no mapped guest memory, no fixture);
    // unmapped_access then stops the machine.
    bool unserved(u32 address, u32 bytes) const noexcept {
        return unmapped_access_stops_ && memory_.peek_pointer(address, bytes) == nullptr && !memory_.device_at(address);
    }
    void unmapped_access(u32 address, u32 bytes, bool write, u64 value);
    // The OS exception entry (os_exception_entry.cpp).
    bool os_vector_installed(Exception vector, u32 index);
    void enter_os_exception(Exception vector);
    void return_from_os_vector() noexcept;
    void translation_fault_read(u32 effective);
    void translation_fault_write(u32 effective);
    void take_memory_fault(u32 effective, bool writing);
    // The first failed data access latches the fault record (see cpu.cpp).
    void latch_memory_fault(u32 effective, bool writing, bool unmapped);
    void restate_memory_fault();
    bool try_hle_slow(u32 address);
    bool lift_site_cut_slow(u32 site);
    // Runs the lowered site at pc, if there is one, and continues after it
    // (the dispatcher's and step()'s path). Returns whether it ran one.
    bool run_lift_site_at_pc();

    GuestMemory& memory_;
    const LiftSiteRecord* lift_sites_ = nullptr;
    std::size_t lift_site_count_ = 0;
    Lifted* lifted_ = nullptr;
    u64 lift_sites_dispatched_ = 0;
    const GxSiteRecord* gx_sites_ = nullptr;
    std::size_t gx_site_count_ = 0;
    GxSiteSink* gx_sink_ = nullptr;
    u64 unlisted_gx_stores_ = 0;
    bool unmapped_access_stops_ = false;
    u64 unmapped_accesses_stopped_ = 0;
    const PlatformSiteRecord* platform_site_table_ = nullptr;
    std::size_t platform_site_table_count_ = 0;
    // The host code address of the last generated store that left the inline
    // and cold paths for the slow one (the unlisted-site report names it).
    const void* cold_store_return_ = nullptr;
    // FM primitives: one volatile host access each (see the accessors).
#if defined(_MSC_VER)
    static GEKKO_ALWAYS_INLINE u8 fm_load8(const u8* p) noexcept {
        return static_cast<u8>(__iso_volatile_load8(reinterpret_cast<const volatile __int8*>(p)));
    }
    static GEKKO_ALWAYS_INLINE u16 fm_load16(const u8* p) noexcept {
        return static_cast<u16>(__iso_volatile_load16(reinterpret_cast<const volatile __int16*>(p)));
    }
    static GEKKO_ALWAYS_INLINE u32 fm_load32(const u8* p) noexcept {
        return static_cast<u32>(__iso_volatile_load32(reinterpret_cast<const volatile __int32*>(p)));
    }
    static GEKKO_ALWAYS_INLINE u64 fm_load64(const u8* p) noexcept {
        return static_cast<u64>(__iso_volatile_load64(reinterpret_cast<const volatile __int64*>(p)));
    }
    static GEKKO_ALWAYS_INLINE void fm_store8(u8* p, u8 v) noexcept {
        __iso_volatile_store8(reinterpret_cast<volatile __int8*>(p), static_cast<__int8>(v));
    }
    static GEKKO_ALWAYS_INLINE void fm_store16(u8* p, u16 v) noexcept {
        __iso_volatile_store16(reinterpret_cast<volatile __int16*>(p), static_cast<__int16>(v));
    }
    static GEKKO_ALWAYS_INLINE void fm_store32(u8* p, u32 v) noexcept {
        __iso_volatile_store32(reinterpret_cast<volatile __int32*>(p), static_cast<__int32>(v));
    }
    static GEKKO_ALWAYS_INLINE void fm_store64(u8* p, u64 v) noexcept {
        __iso_volatile_store64(reinterpret_cast<volatile __int64*>(p), static_cast<__int64>(v));
    }
    static GEKKO_ALWAYS_INLINE u16 fm_swap16(u16 v) noexcept { return _byteswap_ushort(v); }
    static GEKKO_ALWAYS_INLINE u32 fm_swap32(u32 v) noexcept { return _byteswap_ulong(v); }
    static GEKKO_ALWAYS_INLINE u64 fm_swap64(u64 v) noexcept { return _byteswap_uint64(v); }
#else
    static GEKKO_ALWAYS_INLINE u8 fm_load8(const u8* p) noexcept { return *reinterpret_cast<const volatile u8*>(p); }
    static GEKKO_ALWAYS_INLINE u16 fm_load16(const u8* p) noexcept { return *reinterpret_cast<const volatile u16*>(p); }
    static GEKKO_ALWAYS_INLINE u32 fm_load32(const u8* p) noexcept { return *reinterpret_cast<const volatile u32*>(p); }
    static GEKKO_ALWAYS_INLINE u64 fm_load64(const u8* p) noexcept { return *reinterpret_cast<const volatile u64*>(p); }
    static GEKKO_ALWAYS_INLINE void fm_store8(u8* p, u8 v) noexcept { *reinterpret_cast<volatile u8*>(p) = v; }
    static GEKKO_ALWAYS_INLINE void fm_store16(u8* p, u16 v) noexcept { *reinterpret_cast<volatile u16*>(p) = v; }
    static GEKKO_ALWAYS_INLINE void fm_store32(u8* p, u32 v) noexcept { *reinterpret_cast<volatile u32*>(p) = v; }
    static GEKKO_ALWAYS_INLINE void fm_store64(u8* p, u64 v) noexcept { *reinterpret_cast<volatile u64*>(p) = v; }
    static GEKKO_ALWAYS_INLINE u16 fm_swap16(u16 v) noexcept { return __builtin_bswap16(v); }
    static GEKKO_ALWAYS_INLINE u32 fm_swap32(u32 v) noexcept { return __builtin_bswap32(v); }
    static GEKKO_ALWAYS_INLINE u64 fm_swap64(u64 v) noexcept { return __builtin_bswap64(v); }
#endif
    static void fm_register(Cpu* cpu) noexcept;
    static void fm_unregister(Cpu* cpu) noexcept;
    // FM: GuestMemory's two windows and the one the accessors use now.
    u8* fm_window_ = nullptr;
    u8* fm_null_window_ = nullptr;
    u8* fm_base_ = nullptr;
    // False while the journal, the poll watch, an access trace or a latched
    // data fault needs every access on the slow path (select_memory_window):
    // fm_base_ is then the null window, so every access reaches *_slow.
    bool serving_pages_ = true;
    u32 msr_ = 0;
    CpuStop stop_;
    // kInterruptExternal | kInterruptDecrementer. One byte in the slot the
    // external-line bool had, so needs_dispatch/interrupt_pending compile to the
    // same single byte test they always were.
    u8 interrupt_lines_ = 0;
    bool instruction_trace_ = false;
    RecompiledCode* recompiled_ = nullptr;
    // kExitLeave: leave_to_dispatcher ran (pc is the resume address).
    // kExitInterrupt: a condition became deliverable while native code ran; the
    // next exiting() check (always right after a call returned, R2) unwinds.
    static constexpr u8 kExitLeave = 0x01u;
    static constexpr u8 kExitInterrupt = 0x02u;
    // kExitTransfer: an LK=0 transfer was published (transfer_to); the frame
    // waiting for a return runs it (finish_transfers). R12.
    static constexpr u8 kExitTransfer = 0x04u;
    u8 exit_pending_ = 0;
    std::optional<u32> pending_native_call_;
    // (The separate pending_decrementer_ bool that followed is folded into
    // interrupt_lines_; the u32 below keeps its offset.)
    u32 interrupt_fpr_depth_ = 0;
    // The address the outermost pending exception is expected to resume at. If
    // an rfi resumes somewhere else the guest's handler rescheduled, and the
    // saved register file belongs to a thread that is no longer running.
    u32 interrupt_fpr_resume_ = 0;
    // How many times an rfi resumed somewhere other than the interrupted
    // address, i.e. the guest's handler rescheduled and the saved register file
    // was correctly discarded. Non-zero is normal for this game; it is reported
    // so a regression that silently stops discarding is visible.
    u64 interrupt_fpr_context_switches_ = 0;
    FpReg interrupt_fpr_[32]{};
    u32 interrupt_fpscr_ = 0;
    // request_stop sets it; needs_dispatch() reports it at the next back edge;
    // a run ends with it cleared. Atomic because request_stop_from_host raises
    // it from another thread; a relaxed load is the same one-byte load the
    // plain bool was, and the size and offset did not change.
    std::atomic<bool> stop_requested_{false};
    bool record_fallbacks_ = false;
    std::array<u64, static_cast<std::size_t>(FallbackReason::Count)> fallback_totals_{};
    std::map<u64, FallbackSite> fallback_sites_;
    u32 strand_pc_ = 0;
    u32 strand_lr_ = 0;
    u64 strand_start_instruction_ = 0;  // interpreted_instructions_ at strand start
    std::chrono::steady_clock::time_point strand_start_ns_{};
    std::map<u32, InterpreterStrand> interpreter_strands_;
    u64 interpreted_instructions_ = 0;
    u64 interpreted_ns_ = 0;
    // ---- game time state (see time_base()) ---------------------------------
    GameClock clock_;
    u64 time_base_offset_ = 0;  // time base = clock + offset (mttb moves it)
    u32 dec_value_ = 0;         // the value last written to DEC ...
    u64 dec_written_at_ = 0;    // ... at this clock time
    IdleHandler idle_handler_;
    NativeInterruptHandler native_interrupt_handler_;
    NativeInterruptFirst native_interrupt_first_;
    // The last leave_to_dispatcher's site and reason; the dispatcher clears
    // the site before every native dispatch.
    static constexpr u32 kNoLeaveSite = 0xFFFFFFFFu;
    u32 leave_site_ = kNoLeaveSite;
    FallbackReason leave_reason_ = FallbackReason::None;
    PlatformRead platform_read_;
    PlatformWrite platform_write_;
    PlatformValue platform_value_;
    bool interrupt_fpr_shadow_ = false;
    bool verifying_ = false;
    // Per-call verification protocol (see Cpu::try_hle). The dispatcher arms
    // this with the entry address it dispatches; only the thunk AT that
    // address consumes it and runs, and every other thunk reached while
    // verifying is a nested call that hands back as FallbackReason::VerifyCut
    // so the oracle verifies it as its own segment. An interior entry thunk
    // or a range entry runs no try_hle, so nothing consumes the arm and every
    // nested call is cut (a bool once leaked to the first nested thunk).
    // 0: not armed.
    u32 verify_armed_entry_ = 0;
    // True while verify_dispatch replays a segment through the interpreter.
    // Nothing on that path calls try_hle, and step() invokes no HLE handler
    // while it is set: the replay executes the guest instructions the native
    // pass translated, and handlers run only from the dispatcher.
    bool verify_replaying_ = false;
    std::size_t verify_message_limit_ = 0;
    u64 verify_call_limit_ = 0;
    VerifyReport verify_;
    // Reused by every verified segment. Local vectors used to allocate, copy,
    // and free these effects for every call. Capacity grows only from observed
    // writes and is bounded by GuestMemory::kJournalLimit (and eight bytes per entry).
    std::vector<GuestMemory::JournalEntry> verify_native_writes_scratch_;
    std::vector<std::pair<u32, u8>> verify_native_bytes_scratch_;

    std::vector<TraceEntry> history_;
    std::size_t history_head_ = 0;
    std::size_t history_size_ = 0;

    struct HleEntry {
        u32 address;
        HleHandler handler;
        std::string name;
        u64 hits = 0;  // 2026-09-05 audit: calls this handler fully served
    };
    std::vector<HleEntry> hle_;
    const HleEntry* find_hle(u32 address) const;
    // Direct-mapped try_hle filter (see try_hle). 1024 slots = 4 KiB, one
    // cache line touched per call; two hooks in one slot mark it kHleSlotAll.
    static constexpr u32 kHleSlotCount = 1024u;
    static constexpr u32 kHleSlotMask = kHleSlotCount - 1u;
    static constexpr u32 kHleSlotEmpty = 1u;  // not word-aligned: never a call target
    static constexpr u32 kHleSlotAll = 3u;    // not word-aligned: never a call target
    // Recomputed on every hook install, verification switch and fault
    // latch/clear. Stays exact: a slot only ever rejects addresses it proves
    // are not hooked.
    void rebuild_hle_slots() noexcept;

public:
    // ---- lwarx/stwcx. reservation (architected) -------------------------------
    // Set by lwarx on the aligned 32-byte block it loaded from; cancelled by
    // every stwcx. (any address) and by hard reset (750CL Table 4-9). Exception
    // entry does NOT cancel it (the manuals leave that to the OS's own stwcx.
    // at a context switch). Part of ArchitectedState, so --verify-recomp
    // compares it and savestate version 6 carries it. Declared after every
    // member that preceded it so no offset the generated code reaches moved
    // (only the 2026-09-13 audit block below follows it). Not yet
    // cancelled by another bus master's store into the block (DMA snoop).
    bool reservation = false;
    u32 reservation_address = 0;

private:
    // 2026-09-13 core audit. Declared after every other member so no offset
    // the generated code reaches moves.
    MemoryFault memory_fault_;
    u64 hle_slow_calls_ = 0;
    u32 hle_slots_[kHleSlotCount]{};
    // Exact address -> hle_ index. find_hle used to binary-search 112-byte
    // rows on every hooked call and every dispatcher iteration.
    std::unordered_map<u32, std::size_t> hle_index_;
    // Reference interpreter only (step()): the instruction step() just executed
    // ends at a recognition point R2/R3/R4/R5 (see interrupt_lines_), so the
    // next step() takes a deliverable condition before fetching. Cpu::run's R1
    // check does not read it; verify_dispatch clears it so a replay starts as
    // the native pass did, straight after a dispatch that took nothing.
    bool recognise_interrupt_ = false;
    // request_stop_from_host: 0 none, 1 being posted, 2 posted (the
    // dispatcher serves it and returns the state to 0).
    std::atomic<u8> host_stop_state_{0};
    StopReason host_stop_reason_ = StopReason::Running;
    std::string host_stop_detail_;
    void serve_host_stop();
    // poll_point: the register file at the tracked back edge's last visit.
    struct PollRegisters {
        u32 gpr[32];
        u32 cr, xer, ctr, lr, fpscr, msr;
        u64 fpr[64];
    };
    static constexpr std::size_t kPollWatchLimit = 64u;  // writes a waiting iteration may make
    PollRegisters poll_registers_{};
    u32 poll_site_ = 0;  // 0: nothing tracked
    void capture_poll_registers(PollRegisters& out) const noexcept;
    void wait_for_event();
};

static_assert(sizeof(std::atomic<bool>) == 1 && alignof(std::atomic<bool>) == 1,
              "Cpu::stop_requested_ must keep the plain bool's size and offset");

}  // namespace gekko
