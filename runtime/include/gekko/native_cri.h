#pragma once

// CRI middleware's server threads replaced by native calls at their events (docs/NATIVE_THREADS.md, stage B).
//
// TRANSITIONAL BRIDGE (user directive 2026-11-02): it removes CRI's GameCube threads and the scheduler churn around
// them, but the servers it calls are still CRI's translated code. Dependencies it keeps: the Idle server's thread-context
// upcall (NativeInterrupts::request_upcall, continuation 0x80376224), the tick's frame move (relocate_frame), the
// drain's continuation entry 0x80376220, Cpu& register use, CRI's own thread stacks, the HostWork delivery and the scheduler idle wait point. Removal criterion: every callback the four servers
// dispatch is a native C++ service with an oracle test against the original (the server chain map in
// docs/NATIVE_THREADS.md), so the servers themselves become native calls and this file is deleted.
//
// ADXGC init (0x803C5118) created four GameCube threads in CRI's static block B = 0x805ACA10: a Vsync server loop
// (entry 0x803C5454, priority 12: per retrace the svtype 2 server 0x803CC3FC, then resume the Idle thread, then an
// optional user callback), an svtype 4 loop (0x803C53FC, priority 14: per retrace the server 0x803CC36C), an Idle loop
// (0x803C5330, priority 24: the svtype 6 server 0x803CC24C - Sofdec's work - again while it reports work, then
// suspend) and a priority-8 spinner (0x803C5504) that existed only for CRI's lock (0x803C565C / 0x803C55F0: the holder
// raised to priority 1, so no server thread ran until the lock was left).
//
// Here no thread exists. The servers are called at the events that made the threads run:
// - the Vsync and svtype 4 servers (and the user callback) as one tick of PLAIN GUEST CALLS at the end of the retrace
//   delivery (after its wakeup, where priorities 12 / 14 preempted), in interrupt context, on the Vsync thread's own
//   stack (NativeInterrupts::relocate_frame; the console ran them there, and what they leave on a stack is not the
//   interrupted game thread's). This is sound because nothing in them waits for an interrupt any more: the only wait
//   they had, the ADX output's spin at 0x803CF0D4 for an ARQ transfer's DSP DMA completion, is gone with the native
//   output (native_adx_output.cpp, docs/CRI_SERVERS.md 4c); the server chain's reachable code (docs/agents/cri-threads.md)
//   holds no halt / poll wait and no blocking OS call, and System::idle_wait stops the run (NativeCri::note_wait)
//   if one is ever reached inside a tick. If CRI's lock is held the tick waits for the lock's leave (a HostWork
//   delivery raised there runs it, the same way). Several retraces while deferred are one tick: CRI's retrace wait
//   0x8037ECD4 waits for the next change of the counter, so its loop never made up a missed retrace.
// - the Idle server by a HostWork delivery raised at the scheduler's idle wait point (no game thread ready, the only
//   time a priority-24 thread ran), as a thread-context UPCALL (NativeInterrupts::request_upcall): a retrace tick
//   preempted it on the console and still does here (a tick delivered on top of the upcall), calling it again while it
//   reports work and no game thread became ready (the original's preemption, at call granularity).
// - adxm_goto_mwidle_border (0x803C5534): one Idle server call and the callback, synchronously, on the Idle thread's
//   stack (with its drain flag set the Idle loop ran exactly one call; the caller waited for it).
// CRI's lock keeps its count (B+8); its priority words (B+0xC90, B+0xC94) and the spinner's counter (B+0x10) are no
// longer written - only the replaced functions read them (to be confirmed by the guest watch).
// The servers are CRI's own code (translated until lifted); only the thread machinery around them is gone.
// ON by default (A/B in docs/agents/cri-threads.md); SHADOW_NATIVE_CRI=0 keeps the original CRI threads, for diagnosis.

#include <array>
#include <cstdint>
#include <vector>

#include "gekko/types.h"

namespace gekko {

class Cpu;
class NativeInterrupts;

class NativeCri {
public:
    NativeCri(Cpu& cpu, NativeInterrupts& interrupts) : cpu_(cpu), interrupts_(interrupts) {}
    void install();

    // Routed by the thread API owners (native_jobs.cpp): true when the call was CRI's and was served here.
    bool create_thread(Cpu& c);
    bool resume_thread(Cpu& c);
    // The retrace delivery's last phase (NativeVideo::set_retrace_tick): the tick's server calls, one after another as
    // guest calls of the delivery (true while one was just started), on the Vsync thread's stack. Also run by a
    // HostWork delivery for a tick deferred by CRI's lock.
    bool retrace_tick(Cpu& c, u32& frame);
    // System::idle_wait: the guest waits for an event. During a tick that is a bug of this design (an interrupt
    // context cannot be woken by an interrupt): the run stops with the reason. Counted otherwise.
    void note_wait(Cpu& c);
    // Cpu::set_idle_hook: at the scheduler's idle wait point - true when a HostWork delivery was raised.
    bool idle_point(Cpu& c);
    // SelectThread's idle loop head (extract/wait-points.txt), where idle_point acts. Set by the reference build
    // (native_guest_scheduler.cpp); the lifted build has no guest scheduler, so no idle loop to recognise.
    void set_idle_wait_loop(u32 address) noexcept { idle_wait_loop_ = address; }
    // A native wait (os.threads OSSleepThread) is the scheduler idling: what idle_point did at the SelectThread idle spin.
    // Runs the Idle pass now (the Idle server, then the user callback, through NativeInterrupts::run_upcall), or raises the
    // deferred tick's HostWork for the wait to process; false when there is nothing for the Idle thread to do. The only game
    // thread waits, so "a game thread became ready" is `*game_thread_ready` (its wake), not the run queue.
    bool run_idle_in_wait(Cpu& c, const bool* game_thread_ready);

    bool enabled() const noexcept { return enabled_; }
    bool replaced() const noexcept { return replaced_; }
    struct Stats {
        std::uint64_t threads_replaced = 0, vsync_ticks = 0, svtype4_ticks = 0, deferred_ticks = 0,
                      skipped_ticks = 0, idle_calls = 0, idle_passes = 0, idle_preempted = 0, lock_enters = 0,
                      drains = 0, callbacks = 0, host_work_deliveries = 0, waits_in_idle_upcall = 0;
        // Diagnosis of the idle point: every wait point reached, and why it did not raise work.
        std::uint64_t idle_points = 0, idle_points_other_loop = 0, idle_points_thread_set = 0, idle_points_nothing = 0;
    };
    const Stats& stats() const noexcept { return stats_; }
    void save_state(std::vector<u8>& out) const;
    bool load_state(const u8*& cursor, const u8* end);

    // CRI's static block and the addresses this replaces (PAL DOL).
    static constexpr u32 kBlock = 0x805ACA10u;
    static constexpr u32 kVsyncEntry = 0x803C5454u, kSvtype4Entry = 0x803C53FCu, kIdleEntry = 0x803C5330u,
                         kSpinnerEntry = 0x803C5504u;
    static constexpr u32 kLockEnter = 0x803C565Cu, kLockLeave = 0x803C55F0u, kDrain = 0x803C5534u;
    // The synchronous drain's continuation entry (inside the always-served SetInterruptMask's extent;
    // extract/native-owners.txt).
    static constexpr u32 kDrainReturn = 0x80376220u;

private:
    bool host_work(Cpu& c, u32 frame, u32 step);  // the HostWork delivery's handler
    bool upcall(Cpu& c, u32 frame, u32 step);     // the servers, in thread context
    bool tick_work(Cpu& c, u32& frame);
    u32 next_tick_call(Cpu& c, u32& state, u32& r3);
    bool idle_may_run(Cpu& c) const;
    bool game_thread_ready(Cpu& c) const;
    u32 idle_wait_loop_ = 0;
    const bool* wait_ready_ = nullptr;  // run_idle_in_wait: the waiting thread's wake flag
    bool vsync_running(Cpu& c) const;
    bool svtype4_running(Cpu& c) const;
    bool idle_running(Cpu& c) const;
    bool lock_held(Cpu& c) const;
    void end_idle_pass(Cpu& c);  // the Idle loop's tail: drain flag cleared, then suspend (the callback is a call)

    Cpu& cpu_;
    NativeInterrupts& interrupts_;
    bool enabled_ = true;
    bool replaced_ = false;      // ADXGC init created its threads here: the native servers are live
    bool tick_pending_ = false;  // a retrace came while CRI's lock was held
    bool idle_pending_ = false;  // the Idle thread would be resumed: its server has work to look at
    bool tick_active_ = false;   // a tick's upcall is running (a retrace then is skipped, as the original)
    bool idle_active_ = false;   // the Idle server's upcall is running
    std::array<bool, 4> resumed_{};  // the four threads' first resume (returns suspend count 1)
    std::array<u32, 4> stacks_{};    // their stack tops (OSCreateThread's r6): Idle, Vsync, svtype 4, spinner
    // The synchronous drain (adxm_goto_mwidle_border): its phase and where it returns.
    u32 drain_phase_ = 0, drain_lr_ = 0, drain_sp_ = 0;
    Stats stats_;
};

}  // namespace gekko
