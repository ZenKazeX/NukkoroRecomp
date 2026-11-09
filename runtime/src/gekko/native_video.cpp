// Native video. Contract and rationale: include/gekko/native_video.h and
// docs/native-time-20261005/STAGE2_VI.md. Guest addresses below are from the
// disassembly cited there (python -B tools/doldis.py <address>:<count>).

#include "gekko/native_video.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "gekko/cpu.h"
#include "gekko/game_clock.h"
#include "gekko/native_interrupts.h"
#include "gekko/guest_memory.h"
#include "gekko/savestate.h"

namespace gekko {

namespace {

// ---- the entry points the generator routes here (extract/native-owners.txt)
constexpr u32 kViInit = 0x8037E824u;            // VIInit (with __VIInit, the cold start)
constexpr u32 kGetCurrentFieldEvenOdd = 0x8037FF9Cu;  // getCurrentFieldEvenOdd (static)
constexpr u32 kViGetNextField = 0x80380004u;    // VIGetNextField
constexpr u32 kViGetCurrentLine = 0x803800A0u;  // VIGetCurrentLine
constexpr u32 kViFlush = 0x8037FD38u;               // VIFlush
constexpr u32 kViSetNextFrameBuffer = 0x8037FE68u;  // VISetNextFrameBuffer
constexpr u32 kViGetCurrentFrameBuffer = 0x8037FED4u;  // returns the buffer VIFlush made current (r13 + 0x5C18)
constexpr u32 kViSetBlack = 0x8037FEDCu;            // VISetBlack
constexpr u32 kViGetTvFormat = 0x80380138u;         // VIGetTvFormat
constexpr u32 kViConfigure = 0x8037F19Cu;           // VIConfigure
constexpr u32 kViSetPreRetraceCallback = 0x8037E4F0u;  // VISetPreRetraceCallback
constexpr u32 kViSetPostRetraceCallback = 0x8037E534u;  // VISetPostRetraceCallback

// ---- guest functions a retrace delivery calls
constexpr u32 kOsWakeupThread = 0x803793B0u;

// ---- the VI library's small-data words (r13-relative)
constexpr u32 kSdaInitialized = 0x5BC8u;    // IsInitialized
constexpr u32 kSdaRetraceCount = 0x5BCCu;   // retraceCount
constexpr u32 kSdaFlushFlag = 0x5BD0u;      // flushFlag: VIFlush staged a configuration
constexpr u32 kSdaRetraceQueue = 0x5BD4u;   // the OSThreadQueue VIWaitForRetrace sleeps on
constexpr u32 kSdaPreCallback = 0x5BDCu;
constexpr u32 kSdaPostCallback = 0x5BE0u;
constexpr u32 kSdaEncoderType = 0x5BE8u;    // set by VIInit, read by VIConfigure
constexpr u32 kSdaDisplayOffsetH = 0x5BECu; // s16, from SRAM
constexpr u32 kSdaDisplayOffsetV = 0x5BEEu; // s16
constexpr u32 kSdaClearedByInit = 0x5BF0u;  // a word VIInit clears
constexpr u32 kSdaChangeMode = 0x5C00u;     // a staged interlace change waits for the first field
constexpr u32 kSdaCurrTiming = 0x5C10u;
constexpr u32 kSdaCurrTvMode = 0x5C14u;
constexpr u32 kSdaNextBufAddr = 0x5C18u;
constexpr u32 kSdaCurrBufAddr = 0x5C1Cu;
constexpr u32 kSdaFlushedBuffer = 0x5C20u;  // set by VISetNextFrameBuffer
constexpr u32 kSdaConfigureWarned = 0x5C28u;  // VIConfigure's one-time warning flag
constexpr u32 kSdaCustomTiming = 0x5C24u;   // getTiming's indices 28-30

// ---- the VI library's static block (regs[], shdwRegs[], HorVer)
constexpr u32 kViStatic = 0x80587C28u;
constexpr u8 kChangedDisplay = 1u, kChangedClock = 2u;  // changed_ bits
constexpr u8 kStagedDisplay = 1u, kStagedDisplayOn = 2u, kStagedClock = 4u, kStagedClock54 = 8u;  // staged_ bits
constexpr u32 kHorVerDispPosX = 0xF0u;
constexpr u32 kHorVerDispPosY = 0xF2u;
constexpr u32 kHorVerDispSizeX = 0xF4u;
constexpr u32 kHorVerDispSizeY = 0xF6u;
constexpr u32 kHorVerAdjustedDispPosX = 0xF8u;
constexpr u32 kHorVerAdjustedDispPosY = 0xFAu;
constexpr u32 kHorVerAdjustedDispSizeY = 0xFCu;
constexpr u32 kHorVerAdjustedPanPosY = 0xFEu;
constexpr u32 kHorVerAdjustedPanSizeY = 0x100u;
constexpr u32 kHorVerFbSizeX = 0x102u;
constexpr u32 kHorVerFbSizeY = 0x104u;
constexpr u32 kHorVerPanPosX = 0x106u;
constexpr u32 kHorVerPanPosY = 0x108u;
constexpr u32 kHorVerPanSizeX = 0x10Au;
constexpr u32 kHorVerPanSizeY = 0x10Cu;
constexpr u32 kHorVerFbMode = 0x110u;
constexpr u32 kHorVerNonInter = 0x114u;
constexpr u32 kHorVerTv = 0x118u;
constexpr u32 kStaticTiming = 0x144u;       // the configured VITiming
constexpr u32 kStaticBufAddr = 0x120u;      // HorVer.bufAddr, set by VISetNextFrameBuffer
constexpr u32 kStaticBlack = 0x130u;        // the black flag, set by VISetBlack
// The register indices the commit cares about (bit 63 - index of the 64-bit mask).
constexpr u32 kRegDisplayControl = 1u;      // DCR, offset 0x02
constexpr u32 kRegClockSelect = 54u;        // VICLK, offset 0x6C

// ---- VITiming (the SDK's timing records in the DOL, getTiming 0x8037E578)
constexpr u32 kTimingBase = 0x80562DE8u;
constexpr u32 kTimingAcv = 0x02u;
constexpr u32 kTimingNhlines = 0x18u;       // half-lines per field
constexpr u32 kTimingHlw = 0x1Au;           // half-line width in VI pixel clocks

// ---- OS low memory and the OS's SRAM control block (__OSLockSram 0x80377830)
constexpr u32 kOsTvMode = 0x800000CCu;
constexpr u32 kSramControl = 0x80586FE0u;   // sram[64] at +0
constexpr u32 kSramDisplayOffsetH = 0x10u;
constexpr u32 kSramSavedLevel = 0x44u;      // OSDisableInterrupts() saved by the lock
constexpr u32 kSramLocked = 0x48u;

// ---- no register read site is left: the reset path's two DCR writes left with stage 6 (OSResetSystem is owned,
// __OSDoHotReset pruned); the last VISEL reads left with the CARD library (saves.card) and the OS font library
// (os-font: OSGetFontEncode is owned, OSInitFont / OSGetFontTexture never entered)

// The retrace delivery's progress, kept in the delivery frame's scratch word 0.
enum Phase : u32 { kPhaseStart = 0, kPhaseCommit, kPhasePostCallback, kPhaseWakeup, kPhaseTick, kPhaseDone };

const NativeVideo::Address kAddresses[] = {
    {kViInit, "VIInit", false},
    {kGetCurrentFieldEvenOdd, "getCurrentFieldEvenOdd", false},
    {kViGetNextField, "VIGetNextField", false},
    {kViGetCurrentLine, "VIGetCurrentLine", false},
    {kViFlush, "VIFlush", false},
    {kViSetNextFrameBuffer, "VISetNextFrameBuffer", false},
    {kViGetCurrentFrameBuffer, "VIGetCurrentFrameBuffer", false},
    {kViSetBlack, "VISetBlack", false},
    {kViGetTvFormat, "VIGetTvFormat", false},
    {kViConfigure, "VIConfigure", false},
    {kViSetPreRetraceCallback, "VISetPreRetraceCallback", false},
    {kViSetPostRetraceCallback, "VISetPostRetraceCallback", false},
};

const NativeVideo::Site kSites[] = {
    {0x802BEA00u, 0x3800412Bu, "display_field_us"},       // the loading screen's draw: li r0,0x412B
};

std::string hex(u32 value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

void stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "native video: " + why);
}

s32 sext16(u32 value) noexcept { return static_cast<s32>(static_cast<s16>(static_cast<u16>(value))); }

}  // namespace

// The console's 60 Hz field (525 half-lines x 1,287 ticks) and the game's frames in time-base ticks: exactly 1/60 s
// and 1/50 s, where the frame limiter begins each frame (NativeFrameLimiter, one time base).
constexpr u64 kConsoleField60 = 675675u;
constexpr u64 kFrameField60 = kGameTicksPerSecond / 60u;
constexpr u64 kFrameField50 = kGameTicksPerSecond / 50u;
static_assert(kFrameField60 * 60u == kGameTicksPerSecond && kFrameField50 * 50u == kGameTicksPerSecond,
              "whole-tick frames");
static_assert(kFrameField50 == 625u * 1296u, "the 50 Hz console field is already one frame");

NativeVideo::NativeVideo(Cpu& cpu, NativeInterrupts& interrupts) noexcept : cpu_(cpu), interrupts_(interrupts) {}

const NativeVideo::Address* NativeVideo::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

const NativeVideo::Site* NativeVideo::platform_sites(std::size_t& count) noexcept {
    count = sizeof(kSites) / sizeof(kSites[0]);
    return kSites;
}

u64 NativeVideo::field_ticks() const noexcept {
    return static_cast<u64>(nhlines_) * half_line_num_ / half_line_den_;
}

void NativeVideo::reset() noexcept {
    enabled_ = false;
    clock54_ = false;
    staged_ = 0u;
    changed_ = 0u;
    field_index_ = 0;
    origin_ticks_ = 0;
    origin_fraction_ = 0;
    half_line_num_ = 1296u;
    half_line_den_ = 1u;
    nhlines_ = 625u;
    stats_ = Stats{};
}

void NativeVideo::boot(bool pal) { boot_pal_ = pal; }

// getTiming 0x8037E578: a jump table of VITiming records at 0x80562DE8 + n;
// a NULL entry returns 0; indices 28-30 return the custom timing.
u32 NativeVideo::timing_address(Cpu& c, u32 index) {
    constexpr s32 kNone = -1;
    constexpr s32 kCustom = -2;
    static constexpr s32 kOffsets[31] = {
        0x44,  0x6A,  0x128, 0x14E, 0x90,  0xB6,  kNone, kNone, 0xDC,    0x102,   kNone,
        kNone, kNone, kNone, kNone, kNone, 0x90,  0xB6,  kNone, kNone,   0x44,    0x6A,
        kNone, kNone, 0x174, kNone, 0x19A, kNone, kCustom, kCustom, kCustom,
    };
    if (index > 30u) return 0u;
    const s32 offset = kOffsets[index];
    if (offset == kNone) return 0u;
    if (offset == kCustom) return c.mem_read32(c.gpr[13] + kSdaCustomTiming);
    return kTimingBase + static_cast<u32>(offset);
}

void NativeVideo::set_timing(Cpu& c, u32 timing, u32 tv_mode) {
    (void)tv_mode;  // the format follows from the timing's half-line width
    if (timing == 0u) {
        stop(c, "the committed VI timing is NULL (getTiming refused the mode)");
        return;
    }
    // The records are the DOL's read-only data in the program image.
    const u32 record = timing;
    if (!c.guest_memory().mapped(GcAddr(record), kTimingHlw + 2u)) {
        stop(c, "the VI timing " + hex(timing) + " is not mapped guest memory");
        return;
    }
    const u32 nhlines = c.guest_memory().peek16(record + kTimingNhlines);
    const u32 hlw = c.guest_memory().peek16(record + kTimingHlw);
    if (nhlines == 0u || hlw == 0u) {
        stop(c, "the VI timing at " + hex(timing) + " has no field (nhlines " + std::to_string(nhlines) +
                    ", hlw " + std::to_string(hlw) + ")");
        return;
    }
    // A half-line is hlw VI pixel clocks: 13.5 MHz with the 27 MHz clock,
    // 27 MHz with the 54 MHz one; the time base runs at 40.5 MHz.
    nhlines_ = nhlines;
    half_line_num_ = 3u * hlw;
    half_line_den_ = clock54_ ? 2u : 1u;
    // The 60 Hz modes' console field is 525 half-lines of 429 clocks at 13.5 MHz: 675,675 ticks, 59.94 Hz - the
    // NTSC-derived rate of the console's video clock, not a property of the game. The game paces its frames at 60 Hz
    // and also waits on retraces, so a 59.94 Hz retrace drifts through its frames once every 1,001 frames: for ~200
    // frames each third frame waits a whole extra field (measured 2026-10-27: 33 ms game frames at ticks 393-590,
    // 1394-1591, 2392-2592 on every stage, input or not). One time base: the field is exactly 1/60 s (its half-lines
    // evenly spread over it), and frame_locked() fields begin where the frame limiter begins each frame, so the
    // retrace the game waits for is its own frame boundary.
    if (static_cast<u64>(nhlines_) * half_line_num_ == kConsoleField60 * half_line_den_) {
        half_line_num_ = static_cast<u32>(kFrameField60);
        half_line_den_ = nhlines_;
        ++stats_.fields_at_frame_rate;
    }
}

// A field exactly one frame long (1/60 s or 1/50 s): it begins where the frame limiter begins a frame.
bool NativeVideo::frame_locked() const noexcept {
    const u64 field = field_ticks();
    return field_ticks_exact() && (field == kFrameField60 || field == kFrameField50);
}

// The start of the frame-locked field containing game tick `now`: the last whole multiple of the field in the
// time base (the frames' time base; the guest may have moved it from the game clock with mttb).
u64 NativeVideo::grid_field_start(u64 now) const noexcept {
    const u64 field = field_ticks();
    const u64 offset = cpu_.time_base() - cpu_.game_clock().now();
    const u64 start = (now + offset) / field * field - offset;
    return start <= now ? start : now;
}

bool NativeVideo::field_ticks_exact() const noexcept {
    return (static_cast<u64>(nhlines_) * half_line_num_) % half_line_den_ == 0u;
}

void NativeVideo::schedule_next() {
    // The field ends exactly at origin + nhlines half-lines; the event fires
    // at the first whole tick at or after that instant.
    const u64 span = static_cast<u64>(origin_fraction_) + static_cast<u64>(nhlines_) * half_line_num_;
    const u64 whole = span / half_line_den_;
    const u64 fraction = span % half_line_den_;
    cpu_.game_clock().schedule(GameEvent::Retrace, origin_ticks_ + whole + (fraction != 0u ? 1u : 0u));
}

void NativeVideo::start_display(u64 now) {
    enabled_ = true;
    origin_ticks_ = now;
    origin_fraction_ = 0;
    // A frame-locked field begins on the grid: the first one is cut short at the next grid point.
    if (frame_locked())
        origin_ticks_ = grid_field_start(now);
    schedule_next();
}

void NativeVideo::on_retrace(u64 now) {
    if (!enabled_) {
        cpu_.game_clock().cancel(GameEvent::Retrace);
        return;
    }
    // The new field begins exactly where the last one ended.
    const u64 span = static_cast<u64>(origin_fraction_) + static_cast<u64>(nhlines_) * half_line_num_;
    origin_ticks_ += span / half_line_den_;
    origin_fraction_ = static_cast<u32>(span % half_line_den_);
    // After a timing change the field returns to the grid at its first retrace (the retrace tick is a grid point
    // from then on).
    if (frame_locked()) {
        origin_ticks_ = grid_field_start(now);
        origin_fraction_ = 0;
    }
    ++field_index_;
    ++stats_.fields;
    schedule_next();
    interrupts_.raise(NativeInterrupts::Source::Vi);
}

u32 NativeVideo::half_line_in_field(u64 now) const noexcept {
    if (!enabled_ || now < origin_ticks_) return 0u;
    const u64 elapsed = (now - origin_ticks_) * half_line_den_ - origin_fraction_;
    const u64 half_line = elapsed / half_line_num_;
    return half_line >= nhlines_ ? nhlines_ - 1u : static_cast<u32>(half_line);
}

NativeVideo::Beam NativeVideo::beam(u64 now) const noexcept {
    Beam beam;
    beam.frame = field_index_ / 2u;
    beam.field_half_lines = nhlines_;
    beam.half_line = half_line_in_field(now) + ((field_index_ & 1u) != 0u ? nhlines_ : 0u);
    return beam;
}

// __VIRetraceHandler 0x8037E27C, natively: no DI status to acknowledge (the
// delivery did) and no hardware to copy the shadow registers into. The
// position-callback path (DI2/DI3) is unreachable: nothing arms them.
bool NativeVideo::retrace_handler(Cpu& c, u32 frame, u32 step) {
    const u32 sda = c.gpr[13];
    const u32 phase_word = NativeInterrupts::scratch(frame, 0u);
    u32 phase = step == 0u ? static_cast<u32>(kPhaseStart) : c.mem_read32(phase_word);
    for (;;) {
        switch (phase) {
            case kPhaseStart: {
                ++stats_.deliveries;
                c.mem_write32(sda + kSdaRetraceCount, c.mem_read32(sda + kSdaRetraceCount) + 1u);
                // The delivery made its exception context current, cleared:
                // the handler's OSClearContext + OSSetCurrentContext.
                const u32 callback = c.mem_read32(sda + kSdaPreCallback);
                phase = kPhaseCommit;
                if (callback != 0u) {
                    ++stats_.pre_callbacks;
                    c.mem_write32(phase_word, phase);
                    interrupts_.call_guest(c, frame, callback, c.mem_read32(sda + kSdaRetraceCount), 0u);
                    return true;
                }
                break;
            }
            case kPhaseCommit: {
                phase = kPhasePostCallback;
                if (c.mem_read32(sda + kSdaFlushFlag) == 0u) break;
                // changeMode: an interlace change commits on the first field
                // only (getCurrentFieldEvenOdd() != 0: half-line < nhlines).
                if (c.mem_read32(sda + kSdaChangeMode) == 1u && (field_index_ & 1u) != 0u) {
                    ++stats_.gated_commits;
                    break;
                }
                // What the hardware would now run with: the staged clock select
                // and display enable, when staged.
                if ((staged_ & kStagedClock) != 0u) clock54_ = (staged_ & kStagedClock54) != 0u;
                const bool display_off = (staged_ & kStagedDisplay) != 0u && (staged_ & kStagedDisplayOn) == 0u;
                staged_ = 0u;
                c.mem_write32(sda + kSdaChangeMode, 0u);
                const u32 timing = c.mem_read32(kViStatic + kStaticTiming);
                const u32 tv = c.mem_read32(kViStatic + kHorVerTv);
                c.mem_write32(sda + kSdaCurrTiming, timing);
                c.mem_write32(sda + kSdaCurrTvMode, tv);
                c.mem_write32(sda + kSdaCurrBufAddr, c.mem_read32(sda + kSdaNextBufAddr));
                c.mem_write32(sda + kSdaFlushFlag, 0u);
                ++stats_.commits;
                // The committed geometry runs from the next field; the field
                // already begun keeps the deadline it was given.
                set_timing(c, timing, tv);
                if (c.stopped()) return false;
                if (display_off) {
                    enabled_ = false;
                    c.game_clock().cancel(GameEvent::Retrace);
                }
                // The SDK then called SIRefreshSamplingRate to refit the SI
                // poll grid to the committed field rate: controller input has no
                // poll grid (docs/lift-20261006/L1_INPUT.md).
                break;
            }
            case kPhasePostCallback: {
                phase = kPhaseWakeup;
                const u32 callback = c.mem_read32(sda + kSdaPostCallback);
                if (callback != 0u) {
                    ++stats_.post_callbacks;
                    interrupts_.clear_exception_context(c, frame);
                    c.mem_write32(phase_word, phase);
                    interrupts_.call_guest(c, frame, callback, c.mem_read32(sda + kSdaRetraceCount), 0u);
                    return true;
                }
                break;
            }
            case kPhaseWakeup:
                c.mem_write32(phase_word, kPhaseTick);
                interrupts_.call_guest(c, frame, kOsWakeupThread, sda + kSdaRetraceQueue, 0u);
                return true;
            case kPhaseTick:
                // Work that ran right after the retrace's wakeup on the console (native_cri.h: CRI's servers).
                if (retrace_tick_ && retrace_tick_(c, frame)) return true;
                c.mem_write32(NativeInterrupts::scratch(frame, 0u), kPhaseDone);  // the frame may have moved
                return false;
            default:
                return false;
        }
    }
}

// VIInit 0x8037E824 with the cold start __VIInit 0x8037E620, natively. The
// display state VIInit read from DCR is the native display's; the filter
// taps, register 0x70 and the DI0/DI1 acknowledges it wrote were hardware only.
void NativeVideo::vi_init(Cpu& c) {
    const u32 sda = c.gpr[13];
    if (c.mem_read32(sda + kSdaInitialized) != 0u) return;
    // OSRegisterVersion(the VI release string): a report, no state.
    c.mem_write32(sda + kSdaInitialized, 1u);
    c.mem_write32(sda + kSdaEncoderType, 1u);

    // DCR as VIInit read it: the display the IPL left running, or - when it
    // is off - the one the cold start brings up: __VIInit(VI_TVMODE_NTSC_INT)
    // writes the OS TV mode word (0, NTSC) and starts NTSC interlaced timing
    // at the 27 MHz clock.
    u32 format = boot_pal_ ? 1u : 0u;
    if (!enabled_) {
        c.mem_write32(kOsTvMode, 0u);
        format = 0u;
        clock54_ = false;
        set_timing(c, timing_address(c, 0u), 0u);
        if (c.stopped()) return;
        start_display(c.game_clock().now());
    }
    const u32 non_interlaced = 0u;
    const u32 dcr = 0x0001u | (non_interlaced << 2) | (format << 8);

    c.mem_write32(sda + kSdaRetraceCount, 0u);
    changed_ = 0u;
    staged_ = 0u;
    c.mem_write32(sda + kSdaClearedByInit, 0u);
    c.mem_write32(sda + kSdaChangeMode, 0u);
    c.mem_write32(sda + kSdaFlushFlag, 0u);

    // __OSLockSram()->displayOffsetH, then __OSUnlockSram(FALSE): the OS's
    // SRAM copy. A locked SRAM made __OSLockSram return NULL, and VIInit read
    // through it.
    if (c.mem_read32(kSramControl + kSramLocked) != 0u) {
        stop(c, "VIInit found the SRAM locked (__OSLockSram returns NULL and VIInit reads through it)");
        return;
    }
    const s8 offset_h = static_cast<s8>(c.mem_read8(kSramControl + kSramDisplayOffsetH));
    c.mem_write32(kSramControl + kSramSavedLevel, (c.msr() & kMsrEE) != 0u ? 1u : 0u);
    c.mem_write16(sda + kSdaDisplayOffsetV, 0u);
    c.mem_write16(sda + kSdaDisplayOffsetH, static_cast<u16>(static_cast<s16>(offset_h)));

    const u32 vi = kViStatic;
    const u32 non_inter = (dcr >> 2) & 1u;
    u32 tv = (dcr >> 8) & 3u;
    c.mem_write32(vi + kHorVerNonInter, non_inter);
    c.mem_write32(vi + kHorVerTv, tv);
    if (c.mem_read32(kOsTvMode) == 1u && tv == 0u) {
        tv = 5u;  // a PAL console running the NTSC format: EURGB60
        c.mem_write32(vi + kHorVerTv, tv);
    }
    const u32 timing = timing_address(c, (tv == 3u ? 0u : tv) * 4u + c.mem_read32(vi + kHorVerNonInter));
    c.mem_write32(vi + kStaticTiming, timing);
    if (timing == 0u) {
        stop(c, "VIInit: getTiming has no timing for TV mode " + std::to_string(tv));
        return;
    }
    c.mem_write16(vi + 2u * kRegDisplayControl, static_cast<u16>(dcr));
    c.mem_write32(sda + kSdaCurrTiming, timing);
    c.mem_write32(sda + kSdaCurrTvMode, tv);

    // The HorVer geometry, as 0x8037EA8C-0x8037EC68 computes it (s16 fields
    // read back as the code reads them).
    const u32 acv2 = (static_cast<u32>(c.mem_read16(timing + kTimingAcv)) << 1) & 0xFFFEu;
    c.mem_write16(vi + kHorVerDispSizeX, 640u);
    c.mem_write16(vi + kHorVerDispSizeY, static_cast<u16>(acv2));
    {
        const s32 room = 720 - static_cast<s32>(c.mem_read16(vi + kHorVerDispSizeX));
        c.mem_write16(vi + kHorVerDispPosX, static_cast<u16>(room / 2));
    }
    c.mem_write16(vi + kHorVerDispPosY, 0u);
    const s32 offset_h16 = sext16(c.mem_read16(sda + kSdaDisplayOffsetH));
    const s32 offset_v16 = sext16(c.mem_read16(sda + kSdaDisplayOffsetV));
    {
        const s32 limit = 720 - static_cast<s32>(c.mem_read16(vi + kHorVerDispSizeX));
        const s32 x = sext16(c.mem_read16(vi + kHorVerDispPosX)) + offset_h16;
        c.mem_write16(vi + kHorVerAdjustedDispPosX, static_cast<u16>(x > limit ? limit : (x >= 0 ? x : 0)));
    }
    const s32 field_divisor = c.mem_read32(vi + kHorVerFbMode) == 0u ? 2 : 1;
    const u32 disp_pos_y = c.mem_read16(vi + kHorVerDispPosY);
    const s32 parity = static_cast<s32>(disp_pos_y & 1u);
    {
        const s32 y = sext16(disp_pos_y) + offset_v16;
        c.mem_write16(vi + kHorVerAdjustedDispPosY, static_cast<u16>(y > parity ? y : parity));
    }
    const s32 acv_lines = 2 * sext16(c.mem_read16(timing + kTimingAcv));
    const s32 bottom_limit = acv_lines - parity;
    const s32 pos_y = sext16(c.mem_read16(vi + kHorVerDispPosY));
    const u32 size_y_u = c.mem_read16(vi + kHorVerDispSizeY);
    const s32 size_y = sext16(size_y_u);
    const s32 below = pos_y + size_y + offset_v16 - bottom_limit;
    const s32 overflow_bottom = below > 0 ? below : 0;
    const s32 above = pos_y + offset_v16 - parity;
    const s32 overflow_top = above < 0 ? above : 0;
    c.mem_write16(vi + kHorVerAdjustedDispSizeY,
                  static_cast<u16>(static_cast<s32>(size_y_u) + overflow_top - overflow_bottom));
    c.mem_write16(vi + kHorVerAdjustedPanPosY,
                  static_cast<u16>(static_cast<s32>(c.mem_read16(vi + kHorVerPanPosY)) - overflow_top / field_divisor));
    c.mem_write16(vi + kHorVerAdjustedPanSizeY,
                  static_cast<u16>(static_cast<s32>(c.mem_read16(vi + kHorVerPanSizeY)) + overflow_top / field_divisor -
                                   overflow_bottom / field_divisor));
    c.mem_write16(vi + kHorVerFbSizeX, 640u);
    c.mem_write16(vi + kHorVerFbSizeY, static_cast<u16>(acv2));
    c.mem_write16(vi + kHorVerPanPosX, 0u);
    c.mem_write16(vi + kHorVerPanPosY, 0u);
    c.mem_write16(vi + kHorVerPanSizeX, 640u);
    c.mem_write16(vi + kHorVerPanSizeY, static_cast<u16>(acv2));
    c.mem_write32(vi + kHorVerFbMode, 0u);
    c.mem_write8(vi + 0x11Cu, 40u);
    c.mem_write8(vi + 0x11Du, 40u);
    c.mem_write8(vi + 0x11Eu, 40u);
    c.mem_write8(vi + 0x12Cu, 0u);
    c.mem_write32(vi + 0x130u, 1u);
    c.mem_write32(vi + 0x134u, 0u);

    // OSInitThreadQueue(&retraceQueue).
    c.mem_write32(sda + kSdaRetraceQueue + 4u, 0u);
    c.mem_write32(sda + kSdaRetraceQueue, 0u);
    // The DI0/DI1 acknowledges: a retrace latched before VIInit is dropped.
    interrupts_.clear(NativeInterrupts::Source::Vi);
    c.mem_write32(sda + kSdaPreCallback, 0u);
    c.mem_write32(sda + kSdaPostCallback, 0u);
    // __OSSetInterruptHandler(__OS_INTERRUPT_PI_VI, __VIRetraceHandler) is not
    // made: NativeInterrupts delivers the retrace (Source::Vi).
    interrupts_.unmask_os_interrupts(c, 0x00000080u);  // PI_VI
}

// ---- the staging entry points. The SDK kept the VI registers in a RAM image (regs[], 16-bit words from the static
// block's start), a 64-bit "changed" mask (index 0 = the most significant bit; two words in the small-data area)
// naming the registers an operation wrote, and shdwRegs[] (+0x78), the copy VIFlush stages for the next retrace.
// Nothing outside the VI library reads the registers the framebuffer and blanking calls would have written (the
// frame buffer addresses, the vertical timing): the display here is the native present path, so VISetNextFrameBuffer
// and VISetBlack keep only the words game code reads (docs/agents/vi.md); VIFlush stages only the two registers the
// native retrace commit reads (display control and clock select, which VIConfigure writes).

// VIFlush 0x8037FD38: stage the written registers for the next retrace.
void NativeVideo::vi_flush(Cpu& c) {
    static const bool trace = std::getenv("SHADOW_TRACE_VI_BLACK") != nullptr;
    if (trace)
        std::fprintf(stderr, "[vi-black] VIFlush black %u tick %llu field %llu\n", c.mem_read32(kViStatic + kStaticBlack),
                     static_cast<unsigned long long>(c.game_clock().now()), static_cast<unsigned long long>(field_index_));
    const u32 sda = c.gpr[13];
    const u32 vi = kViStatic;
    c.mem_write32(sda + kSdaChangeMode, c.mem_read32(sda + kSdaChangeMode) | c.mem_read32(sda + kSdaClearedByInit));
    c.mem_write32(sda + kSdaClearedByInit, 0u);
    // Only the two registers the retrace commit reads (display control, clock select) are staged; every other
    // register the VI library keeps has no reader (docs/agents/vi.md).
    if ((changed_ & kChangedDisplay) != 0u) {
        staged_ = static_cast<u8>((staged_ & ~kStagedDisplayOn) | kStagedDisplay |
                                  ((c.mem_read16(vi + 2u * kRegDisplayControl) & 1u) != 0u ? kStagedDisplayOn : 0u));
    }
    if ((changed_ & kChangedClock) != 0u) {
        staged_ = static_cast<u8>((staged_ & ~kStagedClock54) | kStagedClock |
                                  ((c.mem_read16(vi + 2u * kRegClockSelect) & 1u) != 0u ? kStagedClock54 : 0u));
    }
    changed_ = 0u;
    c.mem_write32(sda + kSdaFlushFlag, 1u);
    c.mem_write32(sda + kSdaNextBufAddr, c.mem_read32(vi + kStaticBufAddr));
}

// VISetNextFrameBuffer 0x8037FE68(fb): the next buffer VIFlush makes current, and the flag that a buffer was set.
// Retired: setFramebuffer's four buffer-address words (static + 0x124 / 0x128 / 0x13C / 0x140), registers 0x1C-0x2B and
// their changed bits - hardware-only state.
void NativeVideo::vi_set_next_frame_buffer(Cpu& c) {
    c.mem_write32(kViStatic + kStaticBufAddr, c.gpr[3]);
    c.mem_write32(c.gpr[13] + kSdaFlushedBuffer, 1u);
}

// VISetBlack 0x8037FEDC(black): the black flag VIConfigure's vertical timing reads. Retired: setVerticalRegs' write of
// registers 0x00 and 0x0C-0x13 and their changed bits (hardware-only state).
void NativeVideo::vi_set_black(Cpu& c) {
    c.mem_write32(kViStatic + kStaticBlack, c.gpr[3]);
    static const bool trace = std::getenv("SHADOW_TRACE_VI_BLACK") != nullptr;
    if (trace)
        std::fprintf(stderr, "[vi-black] VISetBlack(%u) tick %llu field %llu from %08X\n", c.gpr[3],
                     static_cast<unsigned long long>(c.game_clock().now()), static_cast<unsigned long long>(field_index_),
                     c.lr);
}

bool NativeVideo::black_requested() const { return cpu_.mem_read32(kViStatic + kStaticBlack) != 0u; }

// VISetPreRetraceCallback 0x8037E4F0(callback): installs the callback the retrace delivery calls first
// (retrace_handler, kPhaseStart) and returns the previous one.
void NativeVideo::vi_set_pre_retrace_callback(Cpu& c) {
    const u32 sda = c.gpr[13];
    const u32 previous = c.mem_read32(sda + kSdaPreCallback);
    c.mem_write32(sda + kSdaPreCallback, c.gpr[3]);
    c.gpr[3] = previous;
}

// VISetPostRetraceCallback 0x8037E534(callback): the twin of the pre callback - the callback the retrace delivery calls
// last (retrace_handler, kPhasePostCallback) - returns the previous one. (The original brackets it in
// OSDisableInterrupts / OSRestoreInterrupts; the owner runs atomically.)
void NativeVideo::vi_set_post_retrace_callback(Cpu& c) {
    const u32 sda = c.gpr[13];
    const u32 previous = c.mem_read32(sda + kSdaPostCallback);
    c.mem_write32(sda + kSdaPostCallback, c.gpr[3]);
    c.gpr[3] = previous;
}

// VIConfigure 0x8037F19C(rmode): the game's display mode (a GXRenderModeObj) becomes the VI library's HorVer record, the
// TV mode, the timing and the display-control / clock-select registers. Written here: exactly the words other code reads -
// HorVer position / size / frame-buffer-mode / TV mode words (+0xF0..+0xF7, +0x106..+0x11B: the game reads them), the
// timing (+0x144, the retrace commit), the mode-change flag, and the two registers (static + 2 and + 0x6C: the
// RenderWare video-mode code reads them) with the native "changed" flags VIFlush stages from. Retired: the rest of the
// register image the SDK programmed (horizontal / vertical timing, filter, frame-buffer registers; HorVer's adjusted
// geometry, +0x11C-0x12F and +0x134) and the setFramebuffer / setVerticalRegs calls: hardware-only state, proven
// unread by the guest watch (docs/agents/vi.md). OSDisableInterrupts / OSRestoreInterrupts bracket the original; the
// owner runs atomically. The OSReport warning for an unsupported mode is a log only.
void NativeVideo::vi_configure(Cpu& c) {
    const u32 mode = c.gpr[3];
    const u32 sda = c.gpr[13];
    const u32 vi = kViStatic;
    const u32 tv_field = c.mem_read32(mode) & 3u;
    if (c.mem_read32(vi + kHorVerNonInter) != tv_field) {
        c.mem_write32(sda + kSdaClearedByInit, 1u);
        c.mem_write32(vi + kHorVerNonInter, tv_field);
    }
    const u32 kind = c.mem_read32(mode) >> 2;
    const u32 os_tv = c.mem_read32(kOsTvMode);
    if (kind == 4u && c.mem_read32(sda + kSdaConfigureWarned) == 0u) c.mem_write32(sda + kSdaConfigureWarned, 1u);
    bool valid = false;
    if (os_tv == 0u || os_tv == 2u || os_tv == 6u || os_tv == 7u) {
        valid = kind == 0u || kind == 2u || kind == 6u || kind == 7u;
    } else if (os_tv == 1u || os_tv == 5u) {
        valid = kind == 1u || kind == 5u;
    }
    if (!valid) {
        stop(c, "VIConfigure: render mode kind " + std::to_string(kind) + " does not match the console's TV mode " +
                    std::to_string(os_tv) + " (the SDK's assert)");
        return;
    }
    const u32 tv = (kind == 0u || kind == 2u) ? os_tv : kind;
    c.mem_write32(vi + kHorVerTv, tv);

    const u32 non_inter = c.mem_read32(vi + kHorVerNonInter);
    c.mem_write16(vi + kHorVerDispPosX, c.mem_read16(mode + 0xAu));
    c.mem_write16(vi + kHorVerDispPosY, non_inter == 1u ? static_cast<u16>((c.mem_read16(mode + 0xCu) << 1) & 0xFFFEu)
                                                         : c.mem_read16(mode + 0xCu));
    c.mem_write16(vi + kHorVerDispSizeX, c.mem_read16(mode + 0xEu));
    const u32 fb_size_x = c.mem_read16(mode + 4u), fb_size_y = c.mem_read16(mode + 8u);
    const u32 fb_mode = c.mem_read32(mode + 0x14u);
    c.mem_write32(vi + kHorVerFbMode, fb_mode);
    c.mem_write16(vi + kHorVerPanPosX, 0u);
    c.mem_write16(vi + kHorVerPanPosY, 0u);
    c.mem_write16(vi + kHorVerPanSizeX, static_cast<u16>(fb_size_x));
    c.mem_write16(vi + kHorVerPanSizeY, static_cast<u16>(fb_size_y));
    u32 size_y = fb_size_y;
    if (non_inter != 2u && non_inter != 3u && fb_mode == 0u) size_y = (size_y << 1) & 0xFFFEu;
    c.mem_write16(vi + kHorVerDispSizeY, static_cast<u16>(size_y));

    const u32 timing = timing_address(c, tv * 4u + non_inter);
    c.mem_write32(vi + kStaticTiming, timing);
    if (timing == 0u) {
        stop(c, "VIConfigure: getTiming has no timing for TV mode " + std::to_string(tv) + " / " + std::to_string(non_inter));
        return;
    }
    if (c.mem_read32(sda + kSdaEncoderType) == 0u) c.mem_write32(vi + kHorVerTv, 3u);
    const u32 final_tv = c.mem_read32(vi + kHorVerTv);

    // Display control and clock select.
    u32 dcr = c.mem_read16(vi + 2u * kRegDisplayControl);
    u32 clock = c.mem_read16(vi + 2u * kRegClockSelect);
    if (non_inter == 2u || non_inter == 3u) {
        dcr |= 4u;
        clock |= 1u;
    } else {
        dcr = (dcr & ~4u) | ((non_inter & 1u) << 2);
        clock &= ~1u;
    }
    dcr = (dcr & ~8u) | ((non_inter == 3u ? 1u : 0u) << 3);
    dcr &= ~0x300u;
    if (final_tv >= 1u && final_tv <= 3u) dcr |= final_tv << 8;
    c.mem_write16(vi + 2u * kRegDisplayControl, static_cast<u16>(dcr));
    c.mem_write16(vi + 2u * kRegClockSelect, static_cast<u16>(clock));
    changed_ |= kChangedDisplay | kChangedClock;
}

// VIGetTvFormat 0x80380138: the committed TV mode as the SDK's VI_NTSC / VI_PAL / VI_MPAL / VI_EURGB60 values
// (a jump table over modes 0-7; the original returned its caller's r31 for a larger mode - none is ever committed).
void NativeVideo::vi_get_tv_format(Cpu& c) {
    const u32 mode = c.mem_read32(c.gpr[13] + kSdaCurrTvMode);
    static constexpr s32 kFormat[8] = {0, 1, -1, 0, 1, -1, 0, 0};  // -1: the mode itself
    if (mode > 7u) {
        stop(c, "VIGetTvFormat: committed TV mode " + std::to_string(mode) + " is outside the SDK's table");
        return;
    }
    c.gpr[3] = kFormat[mode] < 0 ? mode : static_cast<u32>(kFormat[mode]);
}

void NativeVideo::install() {
    Cpu& cpu = cpu_;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, std::string("native video: ") + name);
    };
    interrupts_.set_handler(NativeInterrupts::Source::Vi,
                            [this](Cpu& c, u32 frame, u32 step) { return retrace_handler(c, frame, step); });
    own(kViInit, "VIInit", [this](Cpu& c) { vi_init(c); });
    // getCurrentFieldEvenOdd: 1 in the first field of a frame (the SDK's
    // "odd" field, half-line < nhlines), 0 in the second.
    own(kGetCurrentFieldEvenOdd, "getCurrentFieldEvenOdd",
        [this](Cpu& c) { c.gpr[3] = (field_index_ & 1u) == 0u ? 1u : 0u; });
    // VIGetNextField: (first field ? 0 : 1) ^ (HorVer.AdjustedDispPosY & 1),
    // read with interrupts disabled - native code is atomic.
    own(kViGetNextField, "VIGetNextField", [this](Cpu& c) {
        const u32 first = (field_index_ & 1u) == 0u ? 1u : 0u;
        c.gpr[3] = (first ^ 1u) ^ (c.mem_read16(kViStatic + kHorVerAdjustedDispPosY) & 1u);
    });
    // VIGetCurrentLine: the line within the current field.
    own(kViGetCurrentLine, "VIGetCurrentLine",
        [this](Cpu& c) { c.gpr[3] = half_line_in_field(c.game_clock().now()) >> 1; });
    own(kViFlush, "VIFlush", [this](Cpu& c) { vi_flush(c); });
    own(kViSetNextFrameBuffer, "VISetNextFrameBuffer", [this](Cpu& c) { vi_set_next_frame_buffer(c); });
    own(kViGetCurrentFrameBuffer, "VIGetCurrentFrameBuffer",
        [](Cpu& c) { c.gpr[3] = c.mem_read32(c.gpr[13] + kSdaNextBufAddr); });
    own(kViSetBlack, "VISetBlack", [this](Cpu& c) { vi_set_black(c); });
    own(kViGetTvFormat, "VIGetTvFormat", [this](Cpu& c) { vi_get_tv_format(c); });
    own(kViConfigure, "VIConfigure", [this](Cpu& c) { vi_configure(c); });
    own(kViSetPreRetraceCallback, "VISetPreRetraceCallback", [this](Cpu& c) { vi_set_pre_retrace_callback(c); });
    own(kViSetPostRetraceCallback, "VISetPostRetraceCallback", [this](Cpu& c) { vi_set_post_retrace_callback(c); });
    // The IPL's PAL display runs from boot, field 0 starting now: the PAL
    // interlaced standard - 625 half-lines per field, each 432 VI pixel
    // clocks (1,296 ticks) at 27 MHz, the SDK's own PAL record
    // (getTiming(VI_TVMODE_PAL_INT), 0x80562E78) - until the game commits
    // its own timing.
    if (boot_pal_) {
        clock54_ = false;
        nhlines_ = 625u;
        half_line_num_ = 3u * 432u;
        half_line_den_ = 1u;
        start_display(cpu.game_clock().now());
    }
}

u16 NativeVideo::platform_read16(Cpu& c, u32 site, u32 effective) {
    ++stats_.platform_reads;
    std::size_t count = 0;
    const Site* sites = platform_sites(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (sites[i].address != site) continue;
        const std::string kind = sites[i].kind;
        stop(c, "platform site " + hex(site) + " (" + kind + ") read " + hex(effective));
        return 0u;
    }
    stop(c, "unlisted platform site " + hex(site) + " read " + hex(effective));
    return 0u;
}

// The loading screen's draw (fn_802BE9D4) runs at most once per video field: it skips while fewer than
// `li r0,0x412B` = 16,683 microseconds have passed since its last draw - floor(675,675 / 40.5), the console's 59.94 Hz
// field in the game's microsecond clock (OSGetTime -> 0x80051414). The field here is the game's frame on the one time
// base (exactly 1/60 s, 675,000 ticks: 16,666 us; 1/50 s: 20,000 us), so the same floor of the same field is the
// native value. With the console's constant a 1/60 s field never passed the check, and every loading-screen draw
// slipped to the next event (measured 2026-10-27: 20 ms loading frames, four audio passes each).
u32 NativeVideo::platform_value(Cpu& c, u32 site) {
    std::size_t count = 0;
    const Site* sites = platform_sites(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (sites[i].address != site) continue;
        if (std::string(sites[i].kind) == "display_field_us")
            return static_cast<u32>(field_ticks() * 1000000u / kGameTicksPerSecond);
        stop(c, "platform site " + hex(site) + " (" + sites[i].kind + ") is not a value");
        return 0u;
    }
    stop(c, "unlisted platform value site " + hex(site));
    return 0u;
}

void NativeVideo::save_state(std::vector<u8>& out) const {
    state::put<u8>(out, enabled_ ? 1u : 0u);
    state::put<u8>(out, static_cast<u8>((clock54_ ? 1u : 0u) | (staged_ << 1) | (changed_ << 5)));  // bit 0 clock54, 1-4 staged_, 5-6 changed_
    state::put<u8>(out, boot_pal_ ? 1u : 0u);
    state::put<u64>(out, field_index_);
    state::put<u64>(out, origin_ticks_);
    state::put<u32>(out, origin_fraction_);
    state::put<u32>(out, half_line_num_);
    state::put<u32>(out, half_line_den_);
    state::put<u32>(out, nhlines_);
}

bool NativeVideo::load_state(const u8*& cursor, const u8* end) {
    u8 enabled = 0, clock54 = 0, boot_pal = 0;
    u64 field_index = 0, origin_ticks = 0;
    u32 origin_fraction = 0, num = 0, den = 0, nhlines = 0;
    if (!state::get<u8>(cursor, end, enabled) || !state::get<u8>(cursor, end, clock54) ||
        !state::get<u8>(cursor, end, boot_pal) || !state::get<u64>(cursor, end, field_index) ||
        !state::get<u64>(cursor, end, origin_ticks) || !state::get<u32>(cursor, end, origin_fraction) ||
        !state::get<u32>(cursor, end, num) || !state::get<u32>(cursor, end, den) ||
        !state::get<u32>(cursor, end, nhlines)) {
        return false;
    }
    if (num == 0u || den == 0u || nhlines == 0u || origin_fraction >= den) return false;
    enabled_ = enabled != 0u;
    clock54_ = (clock54 & 1u) != 0u;
    staged_ = static_cast<u8>((clock54 >> 1) & 0xFu);
    changed_ = static_cast<u8>((clock54 >> 5) & 0x3u);
    boot_pal_ = boot_pal != 0u;
    field_index_ = field_index;
    origin_ticks_ = origin_ticks;
    origin_fraction_ = origin_fraction;
    half_line_num_ = num;
    half_line_den_ = den;
    nhlines_ = nhlines;
    return true;
}

}  // namespace gekko
