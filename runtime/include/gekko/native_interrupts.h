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
    // HostWork (native_cri.h): native work that must run as guest calls on the game thread - delivered only when no
    // other source is pending (it has no OS interrupt and no rank) and never masked.
    enum class Source : u8 {
        Vi = 0, DspDma = 1, Dsp = 2, AiDma = 3, PeFinish = 4, GxBreakpoint = 5, Card = 6, HostWork = 7, Count
    };
    static constexpr std::size_t kSourceCount = static_cast<std::size_t>(Source::Count);
    static constexpr std::size_t slot(Source source) noexcept { return static_cast<std::size_t>(source); }
    // The OS interrupt number of a source (OS_INTERRUPTMASK(n) = 0x80000000 >> n).
    static u32 os_interrupt(Source source) noexcept;

    explicit NativeInterrupts(Cpu& cpu) noexcept : cpu_(cpu) {
        unmasked_[index(Source::Card)] = true;
        unmasked_[index(Source::HostWork)] = true;
    }

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
    // A source other than `except` is pending and deliverable (a HostWork handler yields to it).
    bool other_pending(Source except) const;
    // The delivery being handled ran work that, on the console, ran as threads of a higher priority than the one it
    // interrupted (native_cri.h): their preemption put the interrupted thread at the TAIL of its priority's run queue,
    // so an equal-priority ready thread ran next. The delivery's end then reschedules with yield semantics
    // (SelectThread(1)) to keep that order. Transitional, with the CRI thread bridge.
    void request_yield_on_finish() noexcept { yield_on_finish_ = true; }
    // True when SelectThread(yield = 1) would leave every OS word as it is (exactly one guest thread, running, nothing ready): the
    // delivery's end then skips it. Public for the contract test.
    bool yield_is_identity(Cpu& c) const;
    // OSLoadContext of a context saved by an exception, in host code (the delivery's return); false for any other context.
    bool restore_interrupted_context(Cpu& c, u32 context);

    // Thread-context upcalls. TRANSITIONAL BRIDGE (native_cri.h; removed with it): work that ran on the console as a
    // thread woken by an interrupt - CRI's servers - must not run in interrupt context: it waits for other interrupts
    // (the ADX output spins at 0x803CF0D4 until the DSP DMA completion of its ARQ transfer clears a flag), and in
    // interrupt context MSR[EE] is clear, so that wait never ends. A handler that requests an upcall gets, at its
    // delivery's end, the upcall handler called in thread context instead: on the interrupted stack, MSR as the
    // interrupted code had it (EE set), with an OSContext of its own made current. Further interrupts are then
    // delivered on top of it as on top of any thread; their reschedule switches no thread (SelectThread refuses while
    // the current context is not the current thread's), as no game thread ran before a higher-priority server slept
    // again. A delivery during an upcall runs on top of it as on top of any thread (a Vsync tick preempting the Idle
    // server is plain guest calls of that delivery, native_cri.h); it may itself request an upcall. When
    // the upcall handler returns false the interrupted context is restored and rescheduled as the delivery would
    // have been (with yield semantics when requested).
    //   kind: the requester's tag, readable as upcall_kind(frame). The handler has the same step contract as Handler
    //   and makes its calls with upcall_call; scratch(frame, i) are its words too.
    //   stack_top: the stack the work ran on (its thread's OSCreateThread stack top), 0 for the interrupted one.
    void request_upcall(u32 kind, u32 stack_top) noexcept {
        upcall_kind_ = kind;
        upcall_stack_ = stack_top;
    }
    // The reference build's guest scheduler (native_guest_scheduler.cpp): called when a delivery's end needs SelectThread, or
    // restores a context the host restore does not serve. Unset (the lifted build): those are loud stops.
    void set_guest_scheduler(std::function<void(Cpu&, bool yield)> select_thread, std::function<void(Cpu&, u32 context)> load_context) {
        guest_select_thread_ = std::move(select_thread);
        guest_load_context_ = std::move(load_context);
    }
    void set_upcall_handler(std::function<bool(Cpu&, u32 frame, u32 step)> handler) {
        upcall_handler_ = std::move(handler);
    }
    void upcall_call(Cpu& c, u32 frame, u32 function, u32 r3, u32 r4);
    static u32 upcall_kind_word(u32 frame) noexcept;

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
    // From a handler only, between its guest calls: continue the delivery on another stack. The handler's frame
    // (back chain, exception OSContext, tags, scratch words, saved state) is copied to the top of the stack ending at
    // `stack_top` (an OSCreateThread stack top), that copy's exception context becomes current (the old one's FPU
    // ownership is dropped) and r1 points to it; the new frame is returned and is the `frame` of every later step
    // (callback_return finishes the delivery on whatever frame r1 names). Used by the CRI vsync tick (native_cri.h),
    // which the console ran on the Vsync thread's own stack.
    u32 relocate_frame(Cpu& c, u32 frame, u32 stack_top);
    static u32 scratch(u32 frame, u32 index) noexcept;
    // The exception OSContext the delivery made current (the handler's frame).
    static u32 exception_context(u32 frame) noexcept;

    // A native wait (os.threads OSSleepThread, docs/agents/cri-threads.md "S5"): process the next pending, unmasked
    // source's native work RIGHT NOW, on the caller's stack, with no exception, no OS context save, no reschedule and no
    // rfi - the source's handler is driven as its step machine: step 0, then every guest call it requests is made with
    // gekko::guest_call (the callback runs to completion; a yield inside it is an ExecutionFault, the seam's limit) and
    // its return starts step n + 1, until the handler reports it finished. The handler's frame is a scratch frame below the
    // caller's r1 at the place deliver() would have put it. MSR[EE] is clear while the work runs (the handlers and the
    // callbacks they call were written for interrupt context, where it always is) and is restored exactly afterwards; the
    // caller's registers other than the volatile ones are untouched.
    // Returns false when no source is pending. A source this cannot drive - HostWork (the CRI bridge's upcalls) - or a
    // handler that asks for an upcall is an ExecutionFault naming it, never skipped; the call then returns true with the
    // machine stopped.
    bool run_pending_event(Cpu& c);
    // The same, for a thread-context upcall (request_upcall's work) run right now: the upcall handler of `kind` is driven as
    // its step machine on the stack ending at `stack_top` (0: below the caller), each upcall_call made with guest_call.
    // Used by the native wait for CRI's Idle pass, which the scheduler's idle loop gave the Idle thread. Returns true when
    // the machine stopped (nothing else is reported); MSR[EE] is clear while it runs and restored after.
    bool run_upcall(Cpu& c, u32 kind, u32 stack_top);
    bool sync_event_active() const noexcept { return sync_; }
    // A native wait (os.threads OSSleepThread) is in progress. The game clock then treats a latched, unmasked source as
    // deliverable whatever MSR[EE] says (System::advance_game_time stops at the first event that raises one, as it does for
    // the scheduler's idle context, where EE was set): events are processed one at a time in clock order, not as a batch ranked
    // by the OS's interrupt priority.
    void set_wait_active(bool active) noexcept { wait_active_ = active; }
    bool wait_active() const noexcept { return wait_active_; }
    static const char* source_name(Source source) noexcept;

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
        u64 reschedules = 0;   // SelectThread entered when a delivery ended
        u64 native_returns = 0; // interrupted contexts restored in host code (no guest OSLoadContext)
        u64 yields_elided = 0; // the preemption yield was the identity (one thread) and SelectThread was not entered
        u64 mask_calls = 0;    // SetInterruptMask served
        u64 upcalls = 0, nested_upcalls = 0, upcall_calls = 0;
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
    // __OSReschedule at a delivery's end (r1 = its dispatch frame): SelectThread, or the interrupted context reloaded.
    void reschedule(Cpu& c, bool yield);
    void begin_upcall(Cpu& c, u32 dispatch, u32 kind, u32 stack_top, bool yield);
    // Drives `handler` (step 0, then one step per guest call it requests) with guest_call; r1 and MSR are the caller's after.
    bool drive_sync(Cpu& c, const std::function<bool(Cpu&, u32, u32)>& handler, u32 frame, const char* what);
    void upcall_return(Cpu& c);
    void end_upcall(Cpu& c, u32 frame);
    void resume(Cpu& c);
    u32 set_interrupt_mask(u32 mask, u32 current);
    // __OSInterruptInit (OSInit, once): the OS's interrupt bookkeeping.
    void interrupt_init(Cpu& c);

    Cpu& cpu_;
    std::array<bool, kSourceCount> status_{};
    std::array<bool, kSourceCount> unmasked_{};
    std::array<Handler, kSourceCount> handlers_{};
    bool yield_on_finish_ = false;
    // run_pending_event: call_guest records the request instead of leaving to the dispatcher.
    struct SyncCall {
        bool valid = false;
        u32 frame = 0, function = 0, r3 = 0, r4 = 0;
    };
    bool sync_ = false;
    bool wait_active_ = false;
    SyncCall sync_call_;
    u32 upcall_kind_ = 0;  // requested by the delivery being handled
    u32 upcall_stack_ = 0;
    std::function<bool(Cpu&, u32 frame, u32 step)> upcall_handler_;
    std::function<void(Cpu&, bool)> guest_select_thread_;
    std::function<void(Cpu&, u32)> guest_load_context_;
    Stats stats_;
};

}  // namespace gekko
