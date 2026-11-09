#pragma once

// A MODEL OF THE GAMECUBE VIDEO INTERFACE'S FIELD TIMING (not native video; NATIVE_AUDIT.md C10, lies list 8).
// It keeps the VI's field clock (half-lines of the committed VITiming, 1,296 time-base ticks each in PAL), the
// beam position the VI library reads back, and the retrace interrupt that drives the translated VI SDK's
// pre/post-retrace callbacks and retrace queue. To be deleted by NE-VIDEO: a host frame clock at
// VIWaitForRetrace / the RenderWare present (NO_EMULATION_PLAN.md section 3).
//
//
// docs/native-time-20261005/STAGE2_VI.md. On the console the VI generated
// fields from its timing registers, latched a display interrupt at the line
// programmed into DI0/DI1, and the SDK's retrace handler counted it, called
// the game's pre-retrace callback, copied the shadow registers VIFlush had
// staged into the hardware, refreshed the SI sampling rate, called the
// post-retrace callback and woke every thread waiting for the next field.
// Here the game clock ends each field at a native deadline (GameEvent::Retrace)
// and NativeInterrupts delivers the retrace (Source::Vi) in interrupt
// context; the delivery is the SDK handler's work with nothing to copy the
// shadow registers into and no SI poll grid to refit (controller input is the
// lifted peripheral layer, docs/lift-20261006/L1_INPUT.md). The VI
// library's own entry points that read hardware (VIInit with its cold start,
// VIGetNextField, VIGetCurrentLine) are native owners; the rest of the library
// never touched hardware and stays game code. Code outside the VI library that read VI configuration reaches
// this class through the listed platform sites (extract/platform-sites.txt).
//
// The field: one retrace per field, at the start of each field (the console's
// DI1 at line 1 and DI0 at nhlines/2 + 1 both fall on a field start), evenly
// spaced. A field lasts timing.nhlines half-lines of the committed timing
// (CurrTiming): 1,296 time-base ticks each in the PAL format, 1,287 in the
// 525-line formats, half that with the 54 MHz clock (progressive). Fields are
// counted from boot (across a display restart); even-numbered fields are the
// first field of a frame (the SDK's "odd" field: half-line < nhlines).

#include <cstddef>
#include <vector>

#include "gekko/types.h"

namespace gekko {

class Cpu;
class NativeInterrupts;

class NativeVideo {
public:
    NativeVideo(Cpu& cpu, NativeInterrupts& interrupts) noexcept;

    // The display the IPL left (BootConfiguration): PAL interlaced and
    // running, or (NTSC) off until VIInit's cold start. Before install().
    void boot(bool pal);

    // Installs the owners and the retrace delivery, and starts the field
    // clock of a running boot display. Before the guest boots.
    void install();

    // GameEvent::Retrace: the current field ended at `now`; the next begins.
    void on_retrace(u64 now);

    // The listed platform sites (Cpu::platform_read16): only reads are left -
    // the reset path's DCR writes went with stage 6 (OSResetSystem is owned).
    u16 platform_read16(Cpu& c, u32 site, u32 effective);
    // A listed console constant: display_field_us, the length of the current
    // field in whole microseconds of the game's clock.
    u32 platform_value(Cpu& c, u32 site);

    // Every guest address the generator must route to an owner here
    // (extract/native-owners.txt), and every platform site it lowers
    // (extract/platform-sites.txt).
    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept;
    struct Site {
        u32 address;      // the instruction
        u32 instruction;  // its word in the DOL
        const char* kind;
    };
    static const Site* platform_sites(std::size_t& count) noexcept;

    struct Stats {
        u64 fields = 0;           // retraces raised (fields begun after the first)
        u64 deliveries = 0;       // retrace deliveries run
        u64 commits = 0;          // staged VI configurations committed
        u64 gated_commits = 0;    // commits held for the first field (changeMode)
        u64 pre_callbacks = 0;
        u64 post_callbacks = 0;
        u64 platform_reads = 0;
        u64 fields_at_frame_rate = 0;  // 60 Hz timings given exactly 1/60 s fields (set_timing)
    };
    const Stats& stats() const noexcept { return stats_; }

    // Host diagnostics. A frame is two fields (what the VI model counted);
    // the field count runs on across a display restart.
    u64 frames() const noexcept { return field_index_ / 2u; }
    // The beam for host probes: the frame, the half-line within it (the
    // second field starts at half_lines_per_field) and the field length.
    struct Beam {
        u64 frame = 0;
        u32 half_line = 0;
        u32 field_half_lines = 0;
    };
    Beam beam(u64 now) const noexcept;
    bool enabled() const noexcept { return enabled_; }
    u64 field_index() const noexcept { return field_index_; }
    u32 half_lines_per_field() const noexcept { return nhlines_; }
    bool progressive_clock() const noexcept { return clock54_; }
    // Field period in time-base ticks (rounded down; the clock keeps the
    // fraction exactly).
    u64 field_ticks() const noexcept;
    // The field is the game's frame period (60 or 50 Hz) and begins on the frame limiter's grid (set_timing).
    bool frame_locked() const noexcept;
    bool field_ticks_exact() const noexcept;
    u64 grid_field_start(u64 now) const noexcept;

    // Savestate (gekko/savestate.h, version 11). The deadline is the game
    // clock's; the SDK state is guest memory.
    void save_state(std::vector<u8>& out) const;
    bool load_state(const u8*& cursor, const u8* end);
    void reset() noexcept;

private:
    bool retrace_handler(Cpu& c, u32 frame, u32 step);
    void vi_init(Cpu& c);
    // The SDK's getTiming(index): the VITiming the SDK keeps in the DOL.
    static u32 timing_address(Cpu& c, u32 index);
    // Starts the field clock at `now` (field 0) with the timing given.
    void start_display(u64 now);
    // Field geometry from a committed VITiming and TV mode.
    void set_timing(Cpu& c, u32 timing, u32 tv_mode);
    void schedule_next();
    // The beam: the half-line within the current field.
    u32 half_line_in_field(u64 now) const noexcept;

    Cpu& cpu_;
    NativeInterrupts& interrupts_;
    bool boot_pal_ = true;
    bool enabled_ = false;
    bool clock54_ = false;
    u64 field_index_ = 0;
    // The current field began at origin_ticks_ + origin_fraction_ / den.
    u64 origin_ticks_ = 0;
    u32 origin_fraction_ = 0;
    // A half-line lasts half_line_num_ / half_line_den_ ticks.
    u32 half_line_num_ = 1296u;
    u32 half_line_den_ = 1u;
    u32 nhlines_ = 625u;
    Stats stats_;
};

}  // namespace gekko
