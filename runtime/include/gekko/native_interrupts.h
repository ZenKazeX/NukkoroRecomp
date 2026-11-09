#pragma once

// Native interrupt delivery: how a native subsystem calls back into the game
// in interrupt context, with no interrupt controller underneath.
//
// docs/native-time-20261005/DESIGN.md. On the console a device latched a
// status bit, the processor interface raised the external interrupt, the OS
// vector saved the interrupted thread into its OSContext, __OSDispatchInterrupt
// decoded the PI cause register and called the OS handler registered for the
// cause, and that handler acknowledged the device and called the SDK callback.
// Here the native subsystem owns the status bit and its consumer owns the rest
// - every step that is game or OS software, none that is hardware:
//
//   source status set (NativeVideo: a field began; NativeAudio: the AI pass
//   ended, an ARAM copy finished, the DSP has a mail; NativeGxSync: a
//   breakpoint was reached, a draw-done command was consumed; NativeCard: a
//   card operation completed)
//                                                          -> the CPU's native line
//   recognition point with MSR[EE] = 1                     -> deliver()
//     the OS context save (what the 0x500 vector and ExternalInterruptHandler
//     store into the current OSContext), OSDisableScheduler, a cleared
//     exception OSContext made current (OSClearContext + OSSetCurrentContext,
//     the FP-ownership protocol every SDK handler follows)
//     the source's handler: the native work of the original OS handler, which
//     may call guest functions (the SDK callback) one after another
//     OSClearContext + OSSetCurrentContext(interrupted), OSEnableScheduler,
//     __OSReschedule (SelectThread when a woken thread is waiting), and
//     OSLoadContext(interrupted): the OS's own restore and rfi.
//
// Nothing here is a register, a cause word or a PI mask: there is no
// interrupt controller and no external interrupt (stage 6,
// docs/native-time-20261005/STAGE6_PI_MI.md). This layer owns the OS's
// interrupt bookkeeping: __OSInterruptInit (the handler-table pointer and its
// cleared table, the cleared masks, every interrupt masked - and no
// external-interrupt handler, so the OS's ExternalInterruptHandler and
// __OSDispatchInterrupt are unreachable and pruned), and the OS's interrupt
// mask (__OSMaskInterrupts / __OSUnmaskInterrupts) through the native owner of
// SetInterruptMask, which applies each OS mask group to the native sources
// here. The groups with no source on this platform apply nothing: the memory
// interface's (0-4), the EXI channels' (9-16; the card's completion is not
// OS-maskable, STAGE5_EXI.md D2), AI_AI (8) and the PI's DI, reset switch,
// error, debug and HSP causes. The OS handlers of sources that do not exist -
// MEMIntrruptHandler, __OSResetSWInterruptHandler and the protection-error
// report of __OSUnhandledException that only the former raises - stay in the
// OS's handler table, which nothing dispatches through; they are owned here
// and refuse (stop) if anything ever enters them.
//
// Priority is the OS's: __OSDispatchInterrupt took the pending interrupt of
// the first row of its table (0x805625C8) that has one, the lowest interrupt
// number within the row. Delivery picks among the native sources by that
// rank, read from the game's own data.
//
// All delivery state lives in guest memory, in tagged frames on the
// interrupted thread's stack, so a savestate taken inside a callback resumes.

#include <array>
#include <cstddef>
#include <functional>
#include <vector>

#include "gekko/types.h"

namespace gekko {

class Cpu;

class NativeInterrupts {
public:
    // The OS interrupts this layer serves: __OS_INTERRUPT_PI_VI (24), _DSP_ARAM
    // (6), _DSP_DSP (7), _DSP_AI (5), _PI_PE_FINISH (19), _PI_CP (17), and the
    // card's completion, ranked as _EXI_0_EXI (9). The enumeration order is the
    // savestate's slot order, not a priority: delivery ranks the sources by the
    // OS's table (VI's row, then PE's, then the row holding ARAM, DSP and the
    // EXI range - in that order -, then DSP_AI's, CP's last). _PI_PE_TOKEN (18)
    // is no source: its handler's only work was a draw-sync callback this image
    // never registers (NativeGxSync), so nothing is delivered for it.
    enum class Source : u8 { Vi = 0, DspDma = 1, Dsp = 2, AiDma = 3, PeFinish = 4, GxBreakpoint = 5, Card = 6, Count };
    static constexpr std::size_t kSourceCount = static_cast<std::size_t>(Source::Count);
    static constexpr std::size_t slot(Source source) noexcept { return static_cast<std::size_t>(source); }
    // The OS interrupt number of a source (OS_INTERRUPTMASK(n) = 0x80000000 >> n).
    static u32 os_interrupt(Source source) noexcept;

    explicit NativeInterrupts(Cpu& cpu) noexcept : cpu_(cpu) { unmasked_[index(Source::Card)] = true; }

    // Installs the owners (__OSInterruptInit, SetInterruptMask, the delivery
    // continuations, the refusing handlers) and the CPU's native interrupt
    // handler. Before the guest boots.
    void install();

    // ---- sources -----------------------------------------------------------
    // The latched status of a source (what the hardware kept in its interrupt
    // bit). raise() by the producer; the delivery acknowledges it on entry, as
    // every original handler did first; clear() serves a guest's explicit
    // acknowledge (__ARClearInterrupt).
    void raise(Source source) noexcept;
    void clear(Source source) noexcept;
    bool status(Source source) const noexcept { return status_[index(source)]; }
    // The OS asked for this source (SetInterruptMask: its OS mask bit clear).
    // The card's completion is always unmasked: no OS mask group owns it.
    bool unmasked(Source source) const noexcept { return unmasked_[index(source)]; }
    // The source the next delivery takes - the pending, unmasked source the
    // OS's priority table ranks first - or Source::Count when none is pending.
    Source next_source() const { return static_cast<Source>(best_pending()); }

    // The handler of a source runs in interrupt context, once with step 0 when
    // the delivery starts and once more with step n + 1 after the n-th guest
    // call it made returned. It returns true when it has just started a guest
    // call (call_guest), false when the delivery is finished.
    //   frame: the delivery's guest stack frame; scratch(frame, i) are four
    //   words the handler may keep across its calls.
    using Handler = std::function<bool(Cpu&, u32 frame, u32 step)>;
    void set_handler(Source source, Handler handler);

    // From a handler only: call a guest function in interrupt context with
    // r3/r4 as arguments; it returns to the delivery continuation.
    void call_guest(Cpu& c, u32 frame, u32 function, u32 r3, u32 r4);
    // From a handler only: OSClearContext on the delivery's exception context
    // (an SDK handler that calls several callbacks clears it between them:
    // the FPU no longer belongs to a context the next callback did not use).
    void clear_exception_context(Cpu& c, u32 frame);
    static u32 scratch(u32 frame, u32 index) noexcept;
    // The exception OSContext the delivery made current (the handler's frame).
    static u32 exception_context(u32 frame) noexcept;

    // __OSUnmaskInterrupts (0x8037656C), for a native owner whose original
    // called it: the OS global mask word loses `mask`, and every group that
    // changes is applied (SetInterruptMask). Returns the previous global mask.
    u32 unmask_os_interrupts(Cpu& c, u32 mask);
    // __OSMaskInterrupts (0x803764E4), likewise: the global mask word gains
    // `mask`. Returns the previous global mask.
    u32 mask_os_interrupts(Cpu& c, u32 mask);

    // Every guest address the generator must route to an owner here
    // (extract/native-owners.txt).
    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept;

    struct Stats {
        std::array<u64, kSourceCount> raised{};
        std::array<u64, kSourceCount> delivered{};
        u64 guest_calls = 0;
        u64 reschedules = 0;   // a woken thread was waiting when a delivery ended
        u64 mask_calls = 0;    // SetInterruptMask served
    };
    const Stats& stats() const noexcept { return stats_; }

    // Savestate (gekko/savestate.h, version 15): the source count, then status
    // and mask bits of every source; a block with another source count is
    // refused. The delivery frames are guest memory.
    void save_state(std::vector<u8>& out) const;
    bool load_state(const u8*& cursor, const u8* end);
    void reset() noexcept;

private:
    static std::size_t index(Source source) noexcept { return static_cast<std::size_t>(source); }
    void update_line() noexcept;
    // The best pending, unmasked source by OS rank; kSourceCount for none.
    std::size_t best_pending() const;
    void deliver(Cpu& c);
    void callback_return(Cpu& c);
    void finish(Cpu& c, u32 frame);
    void resume(Cpu& c);
    u32 set_interrupt_mask(u32 mask, u32 current);
    // __OSInterruptInit (OSInit, once): the OS's interrupt bookkeeping.
    void interrupt_init(Cpu& c);

    Cpu& cpu_;
    std::array<bool, kSourceCount> status_{};
    std::array<bool, kSourceCount> unmasked_{};
    std::array<Handler, kSourceCount> handlers_{};
    Stats stats_;
};

}  // namespace gekko
