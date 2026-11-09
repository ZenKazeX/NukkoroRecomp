// Native GX synchronisation. Contract and rationale: include/gekko/native_gx_sync.h
// and docs/native-time-20261005/STAGE4_GX_SYNC.md. Guest addresses below are
// from the disassembly cited there (python -B tools/doldis.py <address>:<count>);
// the small-data words were proven GX-internal by
// scratch/native-time-20261005-v1/scripts/scan_sda_access.py (inventory:
// scratch/native-time-20261005-v1/stage4/gx-pe-cp-inventory.md, section 4).

#include "gekko/native_gx_sync.h"

#include <cstdio>
#include <string>

#include "gekko/cpu.h"
#include "gekko/native_interrupts.h"
#include "gekko/savestate.h"

namespace gekko {

namespace {

// ---- the entry points the generator routes here (extract/native-owners.txt)
constexpr u32 kGxShutdown = 0x80393754u;        // __GXShutdown (OS reset function, record 0x8056874C)
constexpr u32 kGxSetCpuFifo = 0x80394BDCu;      // GXSetCPUFifo
constexpr u32 kGxSetGpFifo = 0x80394D04u;       // GXSetGPFifo
constexpr u32 kGxGetFifoPtrs = 0x80394EA4u;     // GXGetFifoPtrs
constexpr u32 kGxEnableBreakPt = 0x80394F80u;   // GXEnableBreakPt
constexpr u32 kGxDisableBreakPt = 0x80395034u;  // GXDisableBreakPt
constexpr u32 kGxFifoInit = 0x80395088u;        // __GXFifoInit
constexpr u32 kGxReadDrawSync = 0x803964DCu;    // GXReadDrawSync
constexpr u32 kGxPokeAlphaMode = 0x8039658Cu;
constexpr u32 kGxPokeAlphaRead = 0x803965A0u;
constexpr u32 kGxPokeAlphaUpdate = 0x803965C0u;
constexpr u32 kGxPokeBlendMode = 0x803965D8u;
constexpr u32 kGxPokeColorUpdate = 0x8039663Cu;
constexpr u32 kGxPokeDstAlpha = 0x80396654u;
constexpr u32 kGxPokeDither = 0x80396678u;
constexpr u32 kGxPokeZMode = 0x80396690u;
constexpr u32 kGxPeInit = 0x80396840u;          // __GXPEInit
constexpr u32 kGxClearGpMetric = 0x8039AE90u;   // GXClearGPMetric

// ---- guest functions a delivery calls
constexpr u32 kOsWakeupThread = 0x803793B0u;

// ---- OS data
constexpr u32 kOsCurrentThread = 0x800000E4u;  // what OSGetCurrentThread returns
// OS_INTERRUPTMASK(n) = 0x80000000 >> n: PI_CP 17, PI_PE_TOKEN 18, PI_PE_FINISH 19.
constexpr u32 kOsMaskCp = 0x00004000u;
constexpr u32 kOsMaskPeToken = 0x00002000u;
constexpr u32 kOsMaskPeFinish = 0x00001000u;

// ---- the GX library's small-data words (r13-relative)
constexpr u32 kSdaPeCount = 0x5E08u;        // __GXShutdown's static peCount
constexpr u32 kSdaTimeHigh = 0x5E10u;       // __GXShutdown's static time (OSTime)
constexpr u32 kSdaTimeLow = 0x5E14u;
constexpr u32 kSdaCalledOnce = 0x5E18u;     // __GXShutdown's static calledOnce
constexpr u32 kSdaCpuFifo = 0x5E20u;        // CPUFifo
constexpr u32 kSdaGpFifo = 0x5E24u;         // GPFifo
constexpr u32 kSdaCurrentThread = 0x5E28u;  // __GXCurrentThread
constexpr u32 kSdaCpGpLinked = 0x5E2Cu;     // CPGPLinked (u8)
constexpr u32 kSdaOverflowSuspend = 0x5E30u;  // GXOverflowSuspendInProgress
constexpr u32 kSdaBreakPointCb = 0x5E34u;   // BreakPointCB
constexpr u32 kSdaCurrentBp = 0x5E3Cu;      // __GXCurrentBP
constexpr u32 kSdaTokenCb = 0x5E40u;        // TokenCB (the draw-sync callback)
constexpr u32 kSdaDrawDoneCb = 0x5E44u;     // DrawDoneCB
constexpr u32 kSdaDrawDone = 0x5E48u;       // DrawDone (u8)
constexpr u32 kSdaFinishQueue = 0x5E4Cu;    // FinishQueue (OSThreadQueue)

// ---- __GXData (the word at r2 + 0xFD8)
constexpr u32 kSda2GxData = 0xFD8u;
constexpr u32 kGxCpEnable = 0x08u;          // the SDK's CP control shadow
constexpr u32 kGxCpStatus = 0x0Cu;          // the CP status the handler copies
constexpr u32 kGxCpClear = 0x10u;           // the SDK's CP clear shadow
constexpr u32 kGxAbortWaitCopyOut = 0x5AAu; // abtWaitPECopy (u8)
// The CP control shadow's bits: GP read enable, breakpoint enable, FIFO
// overflow / underflow interrupt enables, GP link, breakpoint interrupt enable.
constexpr u32 kCpReadEnable = 0x01u;
constexpr u32 kCpBreakpointEnable = 0x02u;
constexpr u32 kCpOverflowInterrupt = 0x04u;
constexpr u32 kCpUnderflowInterrupt = 0x08u;
constexpr u32 kCpGpLink = 0x10u;
constexpr u32 kCpBreakpointInterrupt = 0x20u;
// The CP clear shadow's bits: clear the overflow / underflow status.
constexpr u32 kCpClearOverflow = 0x01u;
constexpr u32 kCpClearUnderflow = 0x02u;
// The status the breakpoint branch copies: read idle, command idle, breakpoint
// reached - a GP that consumed everything and stands at the armed address.
constexpr u32 kCpStatusAtBreakpoint = 0x001Cu;

// ---- GXFifoObj
constexpr u32 kFifoSize = 0x08u;
constexpr u32 kFifoReadPtr = 0x14u;
constexpr u32 kFifoWritePtr = 0x18u;
constexpr u32 kFifoCount = 0x1Cu;

// ---- the CP performance-select register the listed platform sites name
constexpr u32 kCpPerformanceSelect = 0xCC000006u;

const NativeGxSync::Address kAddresses[] = {
    {kGxShutdown, "__GXShutdown", false},
    {kGxSetCpuFifo, "GXSetCPUFifo", false},
    {kGxSetGpFifo, "GXSetGPFifo", false},
    {kGxGetFifoPtrs, "GXGetFifoPtrs", false},
    {kGxEnableBreakPt, "GXEnableBreakPt", false},
    {kGxDisableBreakPt, "GXDisableBreakPt", false},
    {kGxFifoInit, "__GXFifoInit", false},
    {kGxReadDrawSync, "GXReadDrawSync", false},
    {kGxPokeAlphaMode, "GXPokeAlphaMode", false},
    {kGxPokeAlphaRead, "GXPokeAlphaRead", false},
    {kGxPokeAlphaUpdate, "GXPokeAlphaUpdate", false},
    {kGxPokeBlendMode, "GXPokeBlendMode", false},
    {kGxPokeColorUpdate, "GXPokeColorUpdate", false},
    {kGxPokeDstAlpha, "GXPokeDstAlpha", false},
    {kGxPokeDither, "GXPokeDither", false},
    {kGxPokeZMode, "GXPokeZMode", false},
    {kGxPeInit, "__GXPEInit", false},
    {kGxClearGpMetric, "GXClearGPMetric", false},
};

// D6: the CP performance-select stores of the two init functions that stay
// game code (GXSetGPMetric's only call, from __GXInitGX with both metrics
// NONE, reaches none of its five; they are emitted all the same).
const NativeGxSync::Site kSites[] = {
    {0x80393FF4u, 0xB1830006u, "gx_performance_select"},  // GXInit: sth r12,6(r3), select 0
    {0x8039A754u, 0xB0050006u, "gx_performance_select"},  // GXSetGPMetric: sth r0,6(r5), the old metric off
    {0x8039AE4Cu, 0xB0030006u, "gx_performance_select"},  // GXSetGPMetric: sth r0,6(r3), select 2
    {0x8039AE5Cu, 0xB0030006u, "gx_performance_select"},  // GXSetGPMetric: sth r0,6(r3), select 3
    {0x8039AE6Cu, 0xB0030006u, "gx_performance_select"},  // GXSetGPMetric: sth r0,6(r3), select 4
    {0x8039AE7Cu, 0xB0030006u, "gx_performance_select"},  // GXSetGPMetric: sth r0,6(r3), select 5
};

std::string hex(u32 value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

void stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "native GX sync: " + why);
}

u32 gx_data(Cpu& c) { return c.mem_read32(c.gpr[2] + kSda2GxData); }

void set_cp_enable(Cpu& c, u32 gx, u32 set, u32 clear) {
    c.mem_write32(gx + kGxCpEnable, (c.mem_read32(gx + kGxCpEnable) & ~clear) | set);
}

// __GXWriteFifoIntEnable(hi, lo) 0x80395150: the overflow and underflow
// interrupt enables in the shadow.
void write_fifo_int_enable(Cpu& c, u32 gx, bool overflow, bool underflow) {
    set_cp_enable(c, gx, (overflow ? kCpOverflowInterrupt : 0u) | (underflow ? kCpUnderflowInterrupt : 0u),
                  kCpOverflowInterrupt | kCpUnderflowInterrupt);
}

// __GXFifoLink(en) 0x8039511C: the GP link bit in the shadow.
void fifo_link(Cpu& c, u32 gx, bool link) {
    set_cp_enable(c, gx, link ? kCpGpLink : 0u, kCpGpLink);
}

// __GXWriteFifoIntReset(hi, lo) 0x80395180: the clear shadow's bits.
void write_fifo_int_reset(Cpu& c, u32 gx, bool overflow, bool underflow) {
    const u32 value = (c.mem_read32(gx + kGxCpClear) & ~(kCpClearOverflow | kCpClearUnderflow)) |
                      (overflow ? kCpClearOverflow : 0u) | (underflow ? kCpClearUnderflow : 0u);
    c.mem_write32(gx + kGxCpClear, value);
}

}  // namespace

NativeGxSync::NativeGxSync(Cpu& cpu, NativeInterrupts& interrupts) noexcept
    : cpu_(cpu), interrupts_(interrupts) {}

const NativeGxSync::Address* NativeGxSync::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

const NativeGxSync::Site* NativeGxSync::platform_sites(std::size_t& count) noexcept {
    count = sizeof(kSites) / sizeof(kSites[0]);
    return kSites;
}

bool NativeGxSync::is_platform_site(u32 address) noexcept {
    for (const Site& site : kSites) {
        if (site.address == address) return true;
    }
    return false;
}

void NativeGxSync::reset() noexcept {
    token_ = 0;
    token_interrupt_enabled_ = false;
    finish_interrupt_enabled_ = false;
    cpu_fifo_write_ = 0;
    gp_fifo_read_ = 0;
    gp_fifo_count_ = 0;
    stats_ = Stats{};
}

// ---- the lowered draw-sync sites (gekko/gx_sites.h) -------------------------

// BP 0x47 (token) / BP 0x48 (token with interrupt), written by GXSetDrawSync.
// The token is complete when it is submitted: everything before it has been
// consumed. The interrupt a BP 0x48 requests (once __GXPEInit enabled it) had
// one job, GXTokenInterruptHandler calling TokenCB(token): with no callback it
// only acknowledged itself, so nothing is delivered. A registered callback is
// refused: GXSetDrawSyncCallback's only caller in this image is __GXShutdown,
// with NULL.
void NativeGxSync::on_draw_sync_token(u16 token, bool interrupt) {
    token_ = token;
    ++stats_.tokens;
    if (input_digest_enabled_) {
        const u8 bytes[3] = {static_cast<u8>(token >> 8), static_cast<u8>(token), static_cast<u8>(interrupt)};
        for (const u8 byte : bytes) input_digest_ = (input_digest_ ^ byte) * 0x100000001B3ull;
    }
    if (!interrupt || !token_interrupt_enabled_) return;
    ++stats_.token_interrupts;
    const u32 callback = cpu_.mem_read32(cpu_.gpr[13] + kSdaTokenCb);
    if (callback != 0u) {
        stop(cpu_, "a draw-sync callback " + hex(callback) +
                       " is registered: GXSetDrawSyncCallback's only caller in this image is __GXShutdown, "
                       "with NULL, so nothing here serves one");
    }
}

// BP 0x45 = 2, written by GXDrawDone: the pipe is drained at once.
void NativeGxSync::on_draw_done() {
    ++stats_.draw_dones;
    if (finish_interrupt_enabled_) interrupts_.raise(NativeInterrupts::Source::PeFinish);
}

// ---- owners --------------------------------------------------------------------

// GXSetCPUFifo(fifo) 0x80394BDC. The CPU FIFO's base / top / write position
// went to registers; only the write position is read back (as the game's
// address, 32-byte aligned as the FIFO's writes are).
void NativeGxSync::set_cpu_fifo(Cpu& c) {
    ++stats_.fifo_binds;
    const u32 sda = c.gpr[13];
    const u32 fifo = c.gpr[3];
    const u32 gx = gx_data(c);
    c.mem_write32(sda + kSdaCpuFifo, fifo);
    cpu_fifo_write_ = c.mem_read32(fifo + kFifoWritePtr) & ~0x1Fu;
    if (fifo == c.mem_read32(sda + kSdaGpFifo)) {
        c.mem_write8(sda + kSdaCpGpLinked, 1u);
        write_fifo_int_reset(c, gx, true, true);
        write_fifo_int_enable(c, gx, true, false);
        fifo_link(c, gx, true);
    } else {
        if (c.mem_read8(sda + kSdaCpGpLinked) != 0u) {
            fifo_link(c, gx, false);
            c.mem_write8(sda + kSdaCpGpLinked, 0u);
        }
        write_fifo_int_enable(c, gx, false, false);
    }
}

// GXSetGPFifo(fifo) 0x80394D04. The GP FIFO's geometry went to CP registers;
// the read position and the read-write distance are read back.
void NativeGxSync::set_gp_fifo(Cpu& c) {
    ++stats_.fifo_binds;
    const u32 sda = c.gpr[13];
    const u32 fifo = c.gpr[3];
    const u32 gx = gx_data(c);
    set_cp_enable(c, gx, 0u, kCpReadEnable);          // __GXFifoReadDisable
    write_fifo_int_enable(c, gx, false, false);
    c.mem_write32(sda + kSdaGpFifo, fifo);
    gp_fifo_read_ = c.mem_read32(fifo + kFifoReadPtr);
    gp_fifo_count_ = c.mem_read32(fifo + kFifoCount);
    if (c.mem_read32(sda + kSdaCpuFifo) == fifo) {
        c.mem_write8(sda + kSdaCpGpLinked, 1u);
        write_fifo_int_enable(c, gx, true, false);
        fifo_link(c, gx, true);
    } else {
        c.mem_write8(sda + kSdaCpGpLinked, 0u);
        write_fifo_int_enable(c, gx, false, false);
        fifo_link(c, gx, false);
    }
    // The original pulses the breakpoint enable and its interrupt enable off
    // and back to the shadow: an armed breakpoint with its interrupt enabled
    // is reached again at once (the GP has consumed everything).
    const u32 enable = c.mem_read32(gx + kGxCpEnable);
    if ((enable & (kCpBreakpointEnable | kCpBreakpointInterrupt)) ==
        (kCpBreakpointEnable | kCpBreakpointInterrupt)) {
        ++stats_.breakpoints;
        interrupts_.raise(NativeInterrupts::Source::GxBreakpoint);
    }
    write_fifo_int_reset(c, gx, true, true);
    set_cp_enable(c, gx, kCpReadEnable, 0u);          // __GXFifoReadEnable
}

// GXGetFifoPtrs(fifo, &rd, &wr) 0x80394EA4.
void NativeGxSync::get_fifo_ptrs(Cpu& c) {
    ++stats_.fifo_reads;
    const u32 sda = c.gpr[13];
    const u32 fifo = c.gpr[3];
    if (fifo == c.mem_read32(sda + kSdaCpuFifo)) {
        c.mem_write32(fifo + kFifoWritePtr, cpu_fifo_write_);
    }
    if (fifo == c.mem_read32(sda + kSdaGpFifo)) {
        c.mem_write32(fifo + kFifoReadPtr, gp_fifo_read_);
        c.mem_write32(fifo + kFifoCount, gp_fifo_count_);
    } else {
        u32 count = c.mem_read32(fifo + kFifoWritePtr) - c.mem_read32(fifo + kFifoReadPtr);
        c.mem_write32(fifo + kFifoCount, count);
        if (static_cast<s32>(count) < 0) {
            count += c.mem_read32(fifo + kFifoSize);
            c.mem_write32(fifo + kFifoCount, count);
        }
    }
    c.mem_write32(c.gpr[4], c.mem_read32(fifo + kFifoReadPtr));
    c.mem_write32(c.gpr[5], c.mem_read32(fifo + kFifoWritePtr));
}

// GXEnableBreakPt(addr) 0x80394F80: the breakpoint and its interrupt enabled
// in the shadow, __GXCurrentBP = addr. Everything written before it has been
// consumed, so it is reached at once.
void NativeGxSync::enable_breakpoint(Cpu& c) {
    const u32 gx = gx_data(c);
    set_cp_enable(c, gx, kCpReadEnable | kCpBreakpointEnable | kCpBreakpointInterrupt, 0u);
    c.mem_write32(c.gpr[13] + kSdaCurrentBp, c.gpr[3]);
    ++stats_.breakpoints;
    interrupts_.raise(NativeInterrupts::Source::GxBreakpoint);
}

// GXDisableBreakPt() 0x80395034: both enables cleared, __GXCurrentBP = 0; a
// reached breakpoint not yet delivered is gone with its interrupt enable.
void NativeGxSync::disable_breakpoint(Cpu& c) {
    ++stats_.breakpoints_disabled;
    const u32 gx = gx_data(c);
    set_cp_enable(c, gx, 0u, kCpBreakpointEnable | kCpBreakpointInterrupt);
    c.mem_write32(c.gpr[13] + kSdaCurrentBp, 0u);
    interrupts_.clear(NativeInterrupts::Source::GxBreakpoint);
}

// __GXFifoInit() 0x80395088. __OSSetInterruptHandler(17, GXCPInterruptHandler)
// is not made: NativeInterrupts delivers the breakpoint.
void NativeGxSync::fifo_init(Cpu& c) {
    interrupts_.unmask_os_interrupts(c, kOsMaskCp);
    const u32 sda = c.gpr[13];
    c.mem_write32(sda + kSdaCurrentThread, c.mem_read32(kOsCurrentThread));
    c.mem_write32(sda + kSdaOverflowSuspend, 0u);
    c.mem_write32(sda + kSdaCpuFifo, 0u);
    c.mem_write32(sda + kSdaGpFifo, 0u);
}

// __GXPEInit() 0x80396840. The token and finish handlers are not registered
// (NativeInterrupts delivers the finish; the token needs no delivery); the OS
// mask still unmasks both interrupts, as the original. Its PE control write
// enabled both interrupts and cleared any latched one.
void NativeGxSync::pe_init(Cpu& c) {
    const u32 queue = c.gpr[13] + kSdaFinishQueue;
    c.mem_write32(queue + 4u, 0u);  // OSInitThreadQueue
    c.mem_write32(queue, 0u);
    interrupts_.unmask_os_interrupts(c, kOsMaskPeToken);
    interrupts_.unmask_os_interrupts(c, kOsMaskPeFinish);
    token_interrupt_enabled_ = true;
    finish_interrupt_enabled_ = true;
    interrupts_.clear(NativeInterrupts::Source::PeFinish);
}

// __GXShutdown(final) 0x80393754, the GX library's OS reset function. It
// settled once the memory interface's PE-request counter had not moved for 10
// ticks; the native GP makes no memory requests a counter could see, so the
// counter is 0 and only the 10 ticks remain (OSResetSystem polls this at a
// fixed point, so game time passes). The final pass clears the three
// callbacks and marks abtWaitPECopy; the pipe padding, the CP control / clear
// writes and __GXAbort's counter wait and FIFO reset pulse have nothing to
// pad, write, wait for or reset.
void NativeGxSync::shutdown(Cpu& c) {
    ++stats_.shutdown_calls;
    const u32 sda = c.gpr[13];
    constexpr u32 kPeRequestCounter = 0u;
    if (c.gpr[3] == 0u) {
        const u64 now = c.time_base();
        if (c.mem_read32(sda + kSdaCalledOnce) == 0u) {
            c.mem_write32(sda + kSdaPeCount, kPeRequestCounter);
            c.mem_write32(sda + kSdaTimeLow, static_cast<u32>(now));
            c.mem_write32(sda + kSdaTimeHigh, static_cast<u32>(now >> 32));
            c.mem_write32(sda + kSdaCalledOnce, 1u);
            c.gpr[3] = 0u;
            return;
        }
        const u64 then = (static_cast<u64>(c.mem_read32(sda + kSdaTimeHigh)) << 32) | c.mem_read32(sda + kSdaTimeLow);
        if (static_cast<s64>(now - then) < 10) {
            c.gpr[3] = 0u;
            return;
        }
        if (kPeRequestCounter != c.mem_read32(sda + kSdaPeCount)) {
            c.mem_write32(sda + kSdaTimeLow, static_cast<u32>(now));
            c.mem_write32(sda + kSdaTimeHigh, static_cast<u32>(now >> 32));
            c.mem_write32(sda + kSdaPeCount, kPeRequestCounter);
            c.gpr[3] = 0u;
            return;
        }
        c.gpr[3] = 1u;
        return;
    }
    c.mem_write32(sda + kSdaBreakPointCb, 0u);  // GXSetBreakPtCallback(NULL)
    c.mem_write32(sda + kSdaTokenCb, 0u);       // GXSetDrawSyncCallback(NULL)
    c.mem_write32(sda + kSdaDrawDoneCb, 0u);    // GXSetDrawDoneCallback(NULL)
    // CP control = 0: no breakpoint, no interrupt.
    interrupts_.clear(NativeInterrupts::Source::GxBreakpoint);
    c.mem_write8(gx_data(c) + kGxAbortWaitCopyOut, 1u);
    c.gpr[3] = 1u;
}

// ---- deliveries ---------------------------------------------------------------

// GXCPInterruptHandler 0x803949C0, OS interrupt 17: the breakpoint branch. The
// underflow and overflow branches need the FIFO to cross a watermark, which a
// FIFO consumed as it is written never does.
bool NativeGxSync::breakpoint_handler(Cpu& c, u32 frame, u32 step) {
    if (step != 0u) return false;
    ++stats_.breakpoint_deliveries;
    const u32 gx = gx_data(c);
    c.mem_write32(gx + kGxCpStatus, kCpStatusAtBreakpoint);
    // The branch is taken only with the breakpoint interrupt enabled in the
    // shadow; every raise is made with it set, and every clear of it
    // (GXDisableBreakPt, the final shutdown) withdraws the raise.
    if ((c.mem_read32(gx + kGxCpEnable) & kCpBreakpointInterrupt) == 0u) return false;
    set_cp_enable(c, gx, 0u, kCpBreakpointInterrupt);
    const u32 callback = c.mem_read32(c.gpr[13] + kSdaBreakPointCb);
    if (callback == 0u) return false;
    ++stats_.breakpoint_callbacks;
    interrupts_.call_guest(c, frame, callback, NativeInterrupts::exception_context(frame), 0u);
    return true;
}

// GXFinishInterruptHandler 0x803967C0, OS interrupt 19: DrawDone = 1,
// DrawDoneCB when set, OSWakeupThread(&FinishQueue).
bool NativeGxSync::finish_handler(Cpu& c, u32 frame, u32 step) {
    if (step != 0u) return false;
    ++stats_.finish_deliveries;
    const u32 sda = c.gpr[13];
    c.mem_write8(sda + kSdaDrawDone, 1u);
    const u32 callback = c.mem_read32(sda + kSdaDrawDoneCb);
    if (callback != 0u) {
        stop(c, "a draw-done callback " + hex(callback) +
                    " is registered: GXSetDrawDoneCallback's only caller in this image is __GXShutdown, "
                    "with NULL, so nothing here serves one");
        return false;
    }
    interrupts_.call_guest(c, frame, kOsWakeupThread, sda + kSdaFinishQueue, 0u);
    return true;
}

void NativeGxSync::install() {
    Cpu& cpu = cpu_;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, std::string("native GX sync: ") + name);
    };
    interrupts_.set_handler(NativeInterrupts::Source::GxBreakpoint,
                            [this](Cpu& c, u32 frame, u32 step) { return breakpoint_handler(c, frame, step); });
    interrupts_.set_handler(NativeInterrupts::Source::PeFinish,
                            [this](Cpu& c, u32 frame, u32 step) { return finish_handler(c, frame, step); });
    own(kGxShutdown, "__GXShutdown", [this](Cpu& c) { shutdown(c); });
    own(kGxSetCpuFifo, "GXSetCPUFifo", [this](Cpu& c) { set_cpu_fifo(c); });
    own(kGxSetGpFifo, "GXSetGPFifo", [this](Cpu& c) { set_gp_fifo(c); });
    own(kGxGetFifoPtrs, "GXGetFifoPtrs", [this](Cpu& c) { get_fifo_ptrs(c); });
    own(kGxEnableBreakPt, "GXEnableBreakPt", [this](Cpu& c) { enable_breakpoint(c); });
    own(kGxDisableBreakPt, "GXDisableBreakPt", [this](Cpu& c) { disable_breakpoint(c); });
    own(kGxFifoInit, "__GXFifoInit", [this](Cpu& c) { fifo_init(c); });
    own(kGxReadDrawSync, "GXReadDrawSync", [this](Cpu& c) {
        ++stats_.draw_sync_reads;
        c.gpr[3] = token_;
    });
    // The CPU's EFB access configuration (PE +0x00..+0x08): this image has no
    // CPU EFB access at all (no GXPeek*/GXPoke* of pixels, no 0xC8xxxxxx base),
    // so there is nothing to configure. __GXInitGX calls each once.
    own(kGxPokeAlphaMode, "GXPokeAlphaMode", [](Cpu&) {});
    own(kGxPokeAlphaRead, "GXPokeAlphaRead", [](Cpu&) {});
    own(kGxPokeAlphaUpdate, "GXPokeAlphaUpdate", [](Cpu&) {});
    own(kGxPokeBlendMode, "GXPokeBlendMode", [](Cpu&) {});
    own(kGxPokeColorUpdate, "GXPokeColorUpdate", [](Cpu&) {});
    own(kGxPokeDstAlpha, "GXPokeDstAlpha", [](Cpu&) {});
    own(kGxPokeDither, "GXPokeDither", [](Cpu&) {});
    own(kGxPokeZMode, "GXPokeZMode", [](Cpu&) {});
    own(kGxPeInit, "__GXPEInit", [this](Cpu& c) { pe_init(c); });
    // The performance counters it cleared have no reader in this image.
    own(kGxClearGpMetric, "GXClearGPMetric", [](Cpu&) {});
}

u16 NativeGxSync::platform_read16(Cpu& c, u32 site, u32 effective) {
    stop(c, "platform site " + hex(site) + " read " + hex(effective) + ": the GX sites are stores");
    return 0u;
}

void NativeGxSync::platform_write16(Cpu& c, u32 site, u32 effective, u16 value) {
    for (const Site& listed : kSites) {
        if (listed.address != site) continue;
        if (effective == kCpPerformanceSelect) {
            // Which GP event the performance counters count: nothing counts.
            ++stats_.platform_writes;
            return;
        }
        stop(c, "platform site " + hex(site) + " (" + listed.kind + ") wrote " + hex(value) + " to " + hex(effective));
        return;
    }
    stop(c, "unlisted platform site " + hex(site) + " wrote " + hex(value) + " to " + hex(effective));
}

void NativeGxSync::save_state(std::vector<u8>& out) const {
    state::put<u16>(out, token_);
    state::put<u8>(out, token_interrupt_enabled_ ? 1u : 0u);
    state::put<u8>(out, finish_interrupt_enabled_ ? 1u : 0u);
    state::put<u32>(out, cpu_fifo_write_);
    state::put<u32>(out, gp_fifo_read_);
    state::put<u32>(out, gp_fifo_count_);
}

bool NativeGxSync::load_state(const u8*& cursor, const u8* end) {
    u16 token = 0;
    u8 token_enabled = 0, finish_enabled = 0;
    u32 cpu_write = 0, gp_read = 0, gp_count = 0;
    if (!state::get<u16>(cursor, end, token) || !state::get<u8>(cursor, end, token_enabled) ||
        !state::get<u8>(cursor, end, finish_enabled) || !state::get<u32>(cursor, end, cpu_write) ||
        !state::get<u32>(cursor, end, gp_read) || !state::get<u32>(cursor, end, gp_count)) {
        return false;
    }
    if (token_enabled > 1u || finish_enabled > 1u) return false;
    token_ = token;
    token_interrupt_enabled_ = token_enabled != 0u;
    finish_interrupt_enabled_ = finish_enabled != 0u;
    cpu_fifo_write_ = cpu_write;
    gp_fifo_read_ = gp_read;
    gp_fifo_count_ = gp_count;
    return true;
}

}  // namespace gekko
