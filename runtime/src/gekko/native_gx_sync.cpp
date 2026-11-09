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
constexpr u32 kGxSetDrawSync = 0x80396428u;     // GXSetDrawSync(token)
constexpr u32 kGxInit = 0x80393A88u;            // GXInit(base, size) -> the library's FIFO object
constexpr u32 kGxDrawDone = 0x803964E8u;        // GXDrawDone: no caller in this image
constexpr u32 kGxFifoObject = 0x805A2EE0u;      // the library's static FIFO object GXInit returns (lis / addi at 0x80393B0C)

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
    {kGxSetDrawSync, "GXSetDrawSync", false},
    {kGxInit, "GXInit", false},
    {kGxDrawDone, "GXDrawDone (refused)", false},
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

// The __GXData image the original GXInit leaves (its tail, __GXInitGX 0x80394088: the default texture / TLUT cache regions
// at +0x208..+0x388, the two region callbacks at +0x4C8 / +0x4CC, the BP register shadows and the transform / pipe
// defaults). Guest GX code that still runs - GXLoadTexObj and GXLoadTlut under the RenderWare driver's texture bind, the
// interpreter's boot - reads these words (the callbacks are called through; a null one jumps to address 0). They are
// constants of the library: offset in __GXData, value. The contract test compares the whole structure with the original's.
constexpr u32 kGxDataImage[][2] = {
    {0x000, 0x00010000}, {0x014, 0x00000200}, {0x01C, 0x41377009}, {0x020, 0x41377009}, {0x024, 0x41377009},
    {0x028, 0x41377009}, {0x02C, 0x41377009}, {0x030, 0x41377009}, {0x034, 0x41377009}, {0x038, 0x41377009},
    {0x03C, 0xC8241209}, {0x040, 0xC8241209}, {0x044, 0xC8241209}, {0x048, 0xC8241209}, {0x04C, 0xC8241209},
    {0x050, 0xC8241209}, {0x054, 0xC8241209}, {0x058, 0xC8241209}, {0x05C, 0x04824120}, {0x060, 0x04824120},
    {0x064, 0x04824120}, {0x068, 0x04824120}, {0x06C, 0x04824120}, {0x070, 0x04824120}, {0x074, 0x04824120},
    {0x078, 0x04824120}, {0x07C, 0x22000606}, {0x080, 0x3CF3CF00}, {0x084, 0x00F3CF3C}, {0x0B0, 0xFFFFFFFF},
    {0x0B4, 0xFFFFFFFF}, {0x0B8, 0x30000000}, {0x0BC, 0x32000000}, {0x0C0, 0x34000000}, {0x0C4, 0x36000000},
    {0x0C8, 0x38000000}, {0x0CC, 0x3A000000}, {0x0D0, 0x3C000000}, {0x0D4, 0x3E000000}, {0x0D8, 0x31000000},
    {0x0DC, 0x33000000}, {0x0E0, 0x35000000}, {0x0E4, 0x37000000}, {0x0E8, 0x39000000}, {0x0EC, 0x3B000000},
    {0x0F0, 0x3D000000}, {0x0F4, 0x3F000000}, {0x0F8, 0x20156156}, {0x0FC, 0x213D5335}, {0x100, 0x28049040},
    {0x104, 0x2905B052}, {0x108, 0x2A06D064}, {0x10C, 0x2B07F076}, {0x110, 0x2C380380}, {0x114, 0x2D380380},
    {0x118, 0x2E380380}, {0x11C, 0x2F380380}, {0x120, 0x27000000}, {0x124, 0x0F000000}, {0x128, 0x25000000},
    {0x12C, 0x26000000}, {0x130, 0xC008FFF8}, {0x134, 0xC2000000}, {0x138, 0xC4000000}, {0x13C, 0xC6000000},
    {0x140, 0xC8000000}, {0x144, 0xCA000000}, {0x148, 0xCC000000}, {0x14C, 0xCE000000}, {0x150, 0xD0000000},
    {0x154, 0xD2000000}, {0x158, 0xD4000000}, {0x15C, 0xD6000000}, {0x160, 0xD8000000}, {0x164, 0xDA000000},
    {0x168, 0xDC000000}, {0x16C, 0xDE000000}, {0x170, 0xC108FFC0}, {0x174, 0xC3000000}, {0x178, 0xC5000000},
    {0x17C, 0xC7000000}, {0x180, 0xC9000000}, {0x184, 0xCB000000}, {0x188, 0xCD000000}, {0x18C, 0xCF000000},
    {0x190, 0xD1000000}, {0x194, 0xD3000000}, {0x198, 0xD5000000}, {0x19C, 0xD7000000}, {0x1A0, 0xD9000000},
    {0x1A4, 0xDB000000}, {0x1A8, 0xDD000000}, {0x1AC, 0xDF000000}, {0x1B0, 0xF6018064}, {0x1B4, 0xF701806E},
    {0x1B8, 0xF8018060}, {0x1BC, 0xF901806C}, {0x1C0, 0xFA018065}, {0x1C4, 0xFB01806D}, {0x1C8, 0xFC01806A},
    {0x1CC, 0xFD01806E}, {0x1D0, 0x410004BC}, {0x1D4, 0x42000000}, {0x1D8, 0x40000017}, {0x1DC, 0x43000040},
    {0x1E0, 0x49000000}, {0x1E4, 0x4A077E7F}, {0x1E8, 0x4D000028}, {0x1EC, 0x00000003}, {0x1FC, 0x00000003},
    {0x204, 0x00004001}, {0x208, 0x8C0D8000}, {0x20C, 0x900D8400}, {0x214, 0x00010000}, {0x218, 0x8D0D8800},
    {0x21C, 0x910D8C00}, {0x224, 0x00010000}, {0x228, 0x8E0D9000}, {0x22C, 0x920D9400}, {0x234, 0x00010000},
    {0x238, 0x8F0D9800}, {0x23C, 0x930D9C00}, {0x244, 0x00010000}, {0x248, 0xAC0DA000}, {0x24C, 0xB00DA400},
    {0x254, 0x00010000}, {0x258, 0xAD0DA800}, {0x25C, 0xB10DAC00}, {0x264, 0x00010000}, {0x268, 0xAE0DB000},
    {0x26C, 0xB20DB400}, {0x274, 0x00010000}, {0x278, 0xAF0DB800}, {0x27C, 0xB30DBC00}, {0x284, 0x00010000},
    {0x288, 0x000D8000}, {0x28C, 0x000DC000}, {0x294, 0x00010000}, {0x298, 0x000DC800}, {0x29C, 0x000D8800},
    {0x2A4, 0x00010000}, {0x2A8, 0x000D9000}, {0x2AC, 0x000DD000}, {0x2B4, 0x00010000}, {0x2B8, 0x000DD800},
    {0x2BC, 0x000D9800}, {0x2C4, 0x00010000}, {0x2C8, 0x000DA000}, {0x2CC, 0x000DC400}, {0x2D4, 0x00010000},
    {0x2D8, 0x000DCC00}, {0x2DC, 0x000DA800}, {0x2E4, 0x00010000}, {0x2E8, 0x000DB000}, {0x2EC, 0x000DD400},
    {0x2F4, 0x00010000}, {0x2F8, 0x000DDC00}, {0x2FC, 0x000DB800}, {0x304, 0x00010000}, {0x308, 0x000D8000},
    {0x30C, 0x000DC000}, {0x314, 0x01010000}, {0x318, 0x000DC800}, {0x31C, 0x000D8800}, {0x324, 0x01010000},
    {0x328, 0x000D9000}, {0x32C, 0x000DD000}, {0x334, 0x01010000}, {0x338, 0x000DD800}, {0x33C, 0x000D9800},
    {0x344, 0x01010000}, {0x348, 0x000DA000}, {0x34C, 0x000DC000}, {0x354, 0x01010000}, {0x358, 0x000DC800},
    {0x35C, 0x000DA800}, {0x364, 0x01010000}, {0x368, 0x000DB000}, {0x36C, 0x000DD000}, {0x374, 0x01010000},
    {0x378, 0x000DD800}, {0x37C, 0x000DB800}, {0x384, 0x01010000}, {0x388, 0x65004200}, {0x398, 0x65004210},
    {0x3A8, 0x65004220}, {0x3B8, 0x65004230}, {0x3C8, 0x65004240}, {0x3D8, 0x65004250}, {0x3E8, 0x65004260},
    {0x3F8, 0x65004270}, {0x408, 0x65004280}, {0x418, 0x65004290}, {0x428, 0x650042A0}, {0x438, 0x650042B0},
    {0x448, 0x650042C0}, {0x458, 0x650042D0}, {0x468, 0x650042E0}, {0x478, 0x650042F0}, {0x488, 0x65010300},
    {0x498, 0x65010340}, {0x4A8, 0x65010380}, {0x4B8, 0x650103C0}, {0x4C8, 0x80393634}, {0x4CC, 0x80393730},
    {0x4D8, 0x00000001}, {0x4E0, 0x3F800000}, {0x4E8, 0xBF800000}, {0x4EC, 0xC0000000}, {0x4FC, 0x44200000},
    {0x500, 0x43F00000}, {0x508, 0x3F800000}, {0x510, 0x4B800000}, {0x514, 0x88300C03}, {0x518, 0x89300C03},
    {0x51C, 0x8A300C03}, {0x520, 0x8B300C03}, {0x524, 0xA8300C03}, {0x528, 0xA9300C03}, {0x52C, 0xAA300C03},
    {0x530, 0xAB300C03}, {0x534, 0x80000090}, {0x538, 0x81000090}, {0x53C, 0x82000090}, {0x540, 0x83000090},
    {0x544, 0xA0000090}, {0x548, 0xA1000090}, {0x54C, 0xA2000090}, {0x550, 0xA3000090}, {0x558, 0x00000001},
    {0x55C, 0x00000002}, {0x560, 0x00000003}, {0x564, 0x00000004}, {0x568, 0x00000005}, {0x56C, 0x00000006},
    {0x570, 0x00000007}, {0x574, 0x000000FF}, {0x578, 0x000000FF}, {0x57C, 0x000000FF}, {0x580, 0x000000FF},
    {0x584, 0x000000FF}, {0x588, 0x000000FF}, {0x58C, 0x000000FF}, {0x590, 0x000000FF}, {0x598, 0x000000FF},
    {0x59C, 0x00000023}, {0x5A0, 0x00000016}, {0x5A8, 0x000101FF}, {0x5AC, 0x0000001F},
};

// GXInit(base, size). The calls the original made of the owners this layer already has, with their arguments, and the
// FIFO object GXInitFifoBase (0x80394AF4) filled: base, end = base + size - 4, size, high watermark = size - 0x4000, low
// watermark = size / 2, read pointer = write pointer = base, count 0.
void NativeGxSync::init(Cpu& c) {
    ++stats_.inits;
    const u32 base = c.gpr[3], size = c.gpr[4];
    const u32 saved_lr = c.lr;
    const auto call = [&c](u32 address, u32 r3) {
        c.gpr[3] = r3;
        c.try_hle(address);
    };
    call(kGxFifoInit, 0u);
    const u32 gx = gx_data(c);
    for (const auto& word : kGxDataImage) c.mem_write32(gx + word[0], word[1]);
    c.mem_write32(kGxFifoObject + 0x00u, base);
    c.mem_write32(kGxFifoObject + 0x04u, base + size - 4u);
    c.mem_write32(kGxFifoObject + 0x08u, size);
    c.mem_write32(kGxFifoObject + 0x0Cu, size - 0x4000u);
    c.mem_write32(kGxFifoObject + 0x10u, size >> 1);
    c.mem_write32(kGxFifoObject + 0x14u, base);
    c.mem_write32(kGxFifoObject + 0x18u, base);
    c.mem_write32(kGxFifoObject + 0x1Cu, 0u);
    call(kGxSetCpuFifo, kGxFifoObject);
    call(kGxSetGpFifo, kGxFifoObject);
    call(kGxPeInit, 0u);
    c.lr = saved_lr;
    c.gpr[3] = kGxFifoObject;
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
    // GXSetDrawSync(token): the original wrote BP 0x48 (token with the interrupt request) and BP 0x47 (token) through the
    // pipe - the two lowered token sites, in that order - behind a dirty-state flush and an interrupt mask / restore
    // pair that have no effect here. The same two completions, now without the sites.
    // GXDrawDone: its only listed entry is a test's name for its draw-done site; no guest code calls it, and its original
    // sleeps on the finish queue until the finish interrupt handler wakes it - nothing here serves that wait.
    own(kGxDrawDone, "GXDrawDone", [](Cpu& c) {
        stop(c, "GXDrawDone was entered from " + hex(c.lr) + ": no guest code calls it in this image, and nothing here "
                "serves its wait on the finish queue");
    });
    // GXInit(base, size) -> the library's FIFO object. Its work, in the original's order: GXSetMisc(1, 0), __GXFifoInit
    // (the CP interrupt unmask, the SDA words), GXInitFifoBase (the object's eight words), GXSetCPUFifo and GXSetGPFifo
    // (this layer's owners), __GXPEInit (the PE interrupts), then the library's default state - __GXData's shadow, the
    // texture regions, the hardware register pointers, the version string, the reset record - which has no reader
    // here (docs/agents/card-saves.md, gxD).
    own(kGxInit, "GXInit", [this](Cpu& c) { init(c); });
    own(kGxSetDrawSync, "GXSetDrawSync", [this](Cpu& c) {
        set_draw_sync(static_cast<u16>(c.gpr[3]));
    });
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
