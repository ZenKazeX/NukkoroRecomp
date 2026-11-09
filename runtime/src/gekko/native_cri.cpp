// CRI middleware's server threads as native calls at their events. Contract: include/gekko/native_cri.h,
// docs/NATIVE_THREADS.md (stage B).

#include "gekko/native_cri.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "gekko/cpu.h"
#include "gekko/native_interrupts.h"
#include "gekko/recomp.h"
#include "gekko/savestate.h"

namespace gekko {

namespace {

// The thread API calls CRI made on its threads (PAL DOL).
constexpr u32 kSuspendThread = 0x80379154u;
constexpr u32 kSetPriority = 0x803794B4u;
constexpr u32 kGetPriority = 0x80379574u;
constexpr u32 kCurrentThread = 0x800000E4u;  // OS_CURRENT_THREAD: 0 while the scheduler idles

// CRI's servers and the optional user callback the Vsync and Idle loops call.
constexpr u32 kVsyncServer = 0x803CC3FCu;    // svtype 2
constexpr u32 kSvtype4Server = 0x803CC36Cu;  // svtype 4
constexpr u32 kIdleServer = 0x803CC24Cu;     // svtype 6 (Sofdec's MWSFSVR_IdleThrdProc runs inside it)
constexpr u32 kUserCallback = 0x80569D38u;   // function; its argument at +4
constexpr u32 kVsyncGlobalCounter = 0x805A6924u;

// Fields of CRI's static block B (offsets read from the loops 0x803C5330..0x803C5710).
constexpr u32 kLockCount = 0x08u, kDrainFlag = 0x0Cu, kVsyncCount = 0x14u, kSvtype4Count = 0x18u, kIdleCount = 0x1Cu;
constexpr u32 kIdleThread = 0x30u, kVsyncThread = 0x348u, kSvtype4Thread = 0x660u, kSpinnerThread = 0x978u;
constexpr u32 kSvtype4Exited = 0xC98u, kSvtype4Run = 0xC9Cu, kVsyncExited = 0xCA0u, kVsyncRun = 0xCA4u;
constexpr u32 kIdleExited = 0xCA8u, kIdleRun = 0xCACu;

// The delivery phases of the HostWork handler (scratch word 0) and of the retrace tick (its own state).
enum : u32 { kTickVsync = 0, kTickAfterVsync, kTickSvtype4, kTickDone };
enum : u32 { kIdleCall = 0, kIdleReturned, kIdleCallback };
// The upcall kinds (NativeInterrupts::request_upcall).
enum : u32 { kUpcallIdle = 2 };
constexpr u32 kSdaRunQueueBits = 0x5AE0u;  // RunQueueBits (SelectThread 0x803788C4): a game thread is ready

void stop(Cpu& c, const std::string& why) { c.request_stop(StopReason::ExecutionFault, "native CRI: " + why); }

u32 thread_index(u32 thread) {
    switch (thread - NativeCri::kBlock) {
        case kIdleThread: return 0u;
        case kVsyncThread: return 1u;
        case kSvtype4Thread: return 2u;
        case kSpinnerThread: return 3u;
        default: return ~0u;
    }
}

}  // namespace

bool NativeCri::vsync_running(Cpu& c) const { return c.mem_read32(kBlock + kVsyncRun) == 1u; }
bool NativeCri::svtype4_running(Cpu& c) const { return c.mem_read32(kBlock + kSvtype4Run) == 1u; }
bool NativeCri::idle_running(Cpu& c) const {
    return c.mem_read32(kBlock + kIdleRun) == 1u && c.mem_read32(kBlock + kIdleExited) == 0u;
}
bool NativeCri::lock_held(Cpu& c) const { return c.mem_read32(kBlock + kLockCount) != 0u; }

// The lifted build (SHADOW_NATIVE_CRI=0 is refused at start-up) serves the CRI thread API without a decline path: every entry the
// callers can pass is one of the four server entries (docs/agents/cri-threads.md "Static closure of the thread API"), the lock and
// drain hooks always run, and an impossible state is a stop, never the guest body.
#if defined(SHADOW_RENDER_LIFTED)
constexpr bool kLifted = true;
#else
constexpr bool kLifted = false;
#endif

bool NativeCri::create_thread(Cpu& c) {
    if (!kLifted && !enabled_) return false;
    const u32 entry = c.gpr[4], thread = c.gpr[3];
    const u32 expected = entry == kVsyncEntry     ? kBlock + kVsyncThread
                         : entry == kSvtype4Entry ? kBlock + kSvtype4Thread
                         : entry == kIdleEntry    ? kBlock + kIdleThread
                         : entry == kSpinnerEntry ? kBlock + kSpinnerThread
                                                  : 0u;
    if (expected == 0u) {
        if (!kLifted) return false;
        stop(c, "OSCreateThread with an entry that is none of the four CRI server entries (statically unreachable)");
        return true;
    }
    if (thread != expected) {
        stop(c, "a CRI server thread created on an unexpected OSThread");
        return true;
    }
    replaced_ = true;
    stacks_[thread_index(thread)] = c.gpr[6];  // OSCreateThread's stack top: the servers run there
    ++stats_.threads_replaced;
    c.gpr[3] = 1u;  // TRUE
    return true;
}

bool NativeCri::resume_thread(Cpu& c) {
    if (!kLifted && (!enabled_ || !replaced_)) return false;
    const u32 index = thread_index(c.gpr[3]);
    if (index == ~0u) {
        if (!kLifted) return false;
        stop(c, "OSResumeThread on a thread that is none of the four CRI server threads (statically unreachable)");
        return true;
    }
    c.gpr[3] = resumed_[index] ? 0u : 1u;  // the previous suspend count (created suspended)
    resumed_[index] = true;
    if (index == 0u) idle_pending_ = true;  // ADXGC init resumes the Idle thread: its loop starts
    return true;
}

// A retrace (after the delivery's wakeup, where the console's priority-12 / 14 server threads became ready).
bool NativeCri::retrace_tick(Cpu& c, u32& frame) {
    if (!enabled_ || !replaced_) return false;
    return tick_work(c, frame);
}

// The tick as plain guest calls of the delivery that begins it. Nothing in them can wait for an interrupt (the ADX
// output no longer spins for a transfer's completion: native_adx_output.cpp), so interrupt context serves; they run
// on the Vsync thread's own stack, as the console's thread did. The tick's progress is the frame's scratch word 1
// (0: not begun; else state + 1), so it survives the move to the Vsync stack and a savestate inside a server.
bool NativeCri::tick_work(Cpu& c, u32& frame) {
    u32 stored = c.mem_read32(NativeInterrupts::scratch(frame, 1u));
    if (stored == 0u) {
        if (tick_active_) {
            // The Vsync thread was still in its server: its retrace wait then waited for the next retrace.
            ++stats_.skipped_ticks;
            return false;
        }
        if (lock_held(c)) {  // the holder ran at priority 1: the servers wait for its leave
            tick_pending_ = true;
            ++stats_.deferred_ticks;
            return false;
        }
        tick_pending_ = false;
        tick_active_ = true;
        // The server threads preempted the running thread: it went to its queue's tail.
        interrupts_.request_yield_on_finish();
        frame = interrupts_.relocate_frame(c, frame, stacks_[1]);  // the Vsync thread's stack
        if (c.stopped()) return false;
        stored = 1u + kTickVsync;
    }
    u32 state = stored - 1u, r3 = 0u;
    const u32 function = next_tick_call(c, state, r3);
    c.mem_write32(NativeInterrupts::scratch(frame, 1u), state + 1u);
    if (function != 0u) {
        interrupts_.clear_exception_context(c, frame);  // as between the SDK handlers' callbacks
        interrupts_.call_guest(c, frame, function, r3, 0u);
        return true;
    }
    tick_active_ = false;
    return false;
}

void NativeCri::note_wait(Cpu& c) {
    if (tick_active_) {
        char text[128];
        std::snprintf(text, sizeof(text),
                      "the guest waits for an event inside a CRI server tick (pc 0x%08X, lr 0x%08X): the tick runs in "
                      "interrupt context, where no interrupt can end the wait", c.pc, c.lr);
        stop(c, text);
    } else if (idle_active_) {
        ++stats_.waits_in_idle_upcall;
    }
}

// One tick of the two vsync-driven loops, in the order their priorities ran them (12, then 14): the next server or
// callback to call, or 0 when the tick is over.
u32 NativeCri::next_tick_call(Cpu& c, u32& state, u32& r3) {
    for (;;) {
        switch (state) {
            case kTickVsync:
                state = kTickAfterVsync;
                if (vsync_running(c)) {
                    ++stats_.vsync_ticks;
                    c.mem_write32(kBlock + kVsyncCount, c.mem_read32(kBlock + kVsyncCount) + 1u);
                    c.mem_write32(kVsyncGlobalCounter, c.mem_read32(kVsyncGlobalCounter) + 1u);
                    r3 = 0u;
                    return kVsyncServer;
                }
                c.mem_write32(kBlock + kVsyncExited, 1u);  // the loop left: its exit flag
                state = kTickSvtype4;
                break;
            case kTickAfterVsync: {
                state = kTickSvtype4;
                if (c.mem_read32(kBlock + kIdleExited) == 0u) {
                    idle_pending_ = true;  // OSResumeThread(the Idle thread)
                    const u32 callback = c.mem_read32(kUserCallback);
                    if (callback != 0u) {
                        ++stats_.callbacks;
                        r3 = c.mem_read32(kUserCallback + 4u);
                        return callback;
                    }
                }
                break;
            }
            case kTickSvtype4:
                state = kTickDone;
                if (svtype4_running(c)) {
                    ++stats_.svtype4_ticks;
                    c.mem_write32(kBlock + kSvtype4Count, c.mem_read32(kBlock + kSvtype4Count) + 1u);
                    r3 = 0u;
                    return kSvtype4Server;
                }
                c.mem_write32(kBlock + kSvtype4Exited, 1u);
                break;
            default:
                return 0u;
        }
    }
}

void NativeCri::end_idle_pass(Cpu& c) {
    if (c.mem_read32(kBlock + kDrainFlag) == 1u) c.mem_write32(kBlock + kDrainFlag, 0u);  // priority restore: none
    idle_pending_ = false;  // OSSuspendThread(itself)
    ++stats_.idle_passes;
}

// The Idle loop may run: its server has work to look at, no game thread is ready (it ran at priority 24, below all
// of them) and neither it nor a tick is running.
bool NativeCri::idle_may_run(Cpu& c) const {
    return idle_pending_ && idle_running(c) && !idle_active_ && !tick_active_ && !lock_held(c) &&
           c.mem_read32(kCurrentThread) == 0u && c.mem_read32(c.gpr[13] + kSdaRunQueueBits) == 0u;
}

// The HostWork delivery (interrupt context, no guest call): a deferred tick, else the Idle loop, as an upcall.
bool NativeCri::host_work(Cpu& c, u32 frame, u32 step) {
    u32 current = frame;
    if (step != 0u) {  // a deferred tick's server returned
        return tick_work(c, current);
    }
    ++stats_.host_work_deliveries;
    if (tick_pending_ && !lock_held(c) && !tick_active_) return tick_work(c, current);
    if (idle_may_run(c)) interrupts_.request_upcall(kUpcallIdle, stacks_[0]);  // the Idle thread's stack
    return false;
}

// The server threads' bodies, in thread context (NativeInterrupts::request_upcall).
bool NativeCri::upcall(Cpu& c, u32 frame, u32 step) {
    const u32 kind = c.mem_read32(NativeInterrupts::upcall_kind_word(frame));
    const u32 phase_word = NativeInterrupts::scratch(frame, 0u);
    if (kind != kUpcallIdle) {
        stop(c, "an upcall of unknown kind " + std::to_string(kind));
        return false;
    }
    u32 phase = step == 0u ? static_cast<u32>(kIdleCall) : c.mem_read32(phase_word);
    if (step == 0u) idle_active_ = true;
    for (;;) {
        switch (phase) {
            case kIdleCall:
                ++stats_.idle_calls;
                c.mem_write32(kBlock + kIdleCount, c.mem_read32(kBlock + kIdleCount) + 1u);
                c.mem_write32(phase_word, kIdleReturned);
                interrupts_.upcall_call(c, frame, kIdleServer, 0u, 0u);
                return true;
            case kIdleReturned:
                if (c.gpr[3] != 0u && c.mem_read32(kBlock + kDrainFlag) != 1u) {
                    // More work: the loop calls the server again - unless a game thread became ready (it outranked
                    // the Idle thread; preemption at call granularity) or CRI's lock was taken.
                    if (game_thread_ready(c) || lock_held(c)) {
                        ++stats_.idle_preempted;
                        idle_active_ = false;
                        return false;  // idle stays pending; the next idle point continues
                    }
                    phase = kIdleCall;
                    break;
                }
                if (c.mem_read32(kBlock + kDrainFlag) == 1u) c.mem_write32(kBlock + kDrainFlag, 0u);
                if (const u32 callback = c.mem_read32(kUserCallback); callback != 0u) {
                    ++stats_.callbacks;
                    c.mem_write32(phase_word, kIdleCallback);
                    interrupts_.upcall_call(c, frame, callback, c.mem_read32(kUserCallback + 4u), 0u);
                    return true;
                }
                phase = kIdleCallback;
                break;
            default:  // kIdleCallback
                end_idle_pass(c);
                idle_active_ = false;
                return false;
        }
    }
}

bool NativeCri::game_thread_ready(Cpu& c) const {
    return wait_ready_ != nullptr ? *wait_ready_ : c.mem_read32(c.gpr[13] + kSdaRunQueueBits) != 0u;
}

bool NativeCri::run_idle_in_wait(Cpu& c, const bool* game_thread_ready_flag) {
    if (!enabled_ || !replaced_) return false;
    ++stats_.idle_points;
    if (tick_pending_ && !lock_held(c) && !tick_active_) {  // a deferred tick: the HostWork delivery runs it
        interrupts_.raise(NativeInterrupts::Source::HostWork);
        return true;
    }
    // idle_may_run without its "no game thread is current / ready" terms: the only game thread is the one waiting.
    if (!(idle_pending_ && idle_running(c) && !idle_active_ && !tick_active_ && !lock_held(c))) {
        ++stats_.idle_points_nothing;
        return false;
    }
    wait_ready_ = game_thread_ready_flag;
    (void)interrupts_.run_upcall(c, kUpcallIdle, stacks_[0]);  // the Idle thread's stack
    wait_ready_ = nullptr;
    return true;
}

bool NativeCri::idle_point(Cpu& c) {
    if (!enabled_ || !replaced_) return false;
    ++stats_.idle_points;
    if (idle_wait_loop_ == 0u || c.pc != idle_wait_loop_) {  // 0: no guest scheduler in this build
        ++stats_.idle_points_other_loop;
#if !defined(GEKKO_SHIPPING)
        static u32 logged[8] = {};
        for (u32& slot : logged) {  // diagnosis: the first distinct other wait loops, with the current thread
            if (slot == c.pc) break;
            if (slot == 0u) {
                slot = c.pc;
                std::fprintf(stderr, "[native CRI] wait point at 0x%08X (lr 0x%08X, thread 0x%08X)\n", c.pc, c.lr,
                             c.mem_read32(kCurrentThread));
                break;
            }
        }
#endif
        return false;
    }
    if (c.mem_read32(kCurrentThread) != 0u) {
        ++stats_.idle_points_thread_set;
        return false;
    }
    if (!idle_may_run(c) && !tick_pending_) {
        ++stats_.idle_points_nothing;
        return false;
    }
    interrupts_.raise(NativeInterrupts::Source::HostWork);
    return true;
}

void NativeCri::install() {
    // ON by default (2026-11-07). The 2026-11-02 bridge stalled the stage load (the servers ran in interrupt context);
    // the thread-free tick of docs/CRI_SERVERS.md 5b replaced it, and 120 s A/B replays of Westopolis, Final Haunt and
    // Lethal Highway (off / on / off / on) end with the same game ticks, frames, PCM and AI census; the differing words are
    // classified in docs/agents/cri-threads.md. SHADOW_NATIVE_CRI=0 selects the original CRI threads (diagnosis).
    const char* env = std::getenv("SHADOW_NATIVE_CRI");
    enabled_ = env == nullptr || env[0] != '0';
    Cpu& cpu = cpu_;
    interrupts_.set_handler(NativeInterrupts::Source::HostWork,
                            [this](Cpu& c, u32 frame, u32 step) { return host_work(c, frame, step); });
    interrupts_.set_upcall_handler([this](Cpu& c, u32 frame, u32 step) { return upcall(c, frame, step); });
    // CRI's lock: the count alone (one game thread: nothing to exclude); its leave to 0 runs a deferred tick.
    cpu.install_hle(
        kLockEnter,
        [this](Cpu& c) {
            if (!kLifted && !enabled_) return false;
            ++stats_.lock_enters;
            c.mem_write32(kBlock + kLockCount, c.mem_read32(kBlock + kLockCount) + 1u);
            return true;
        },
        "native CRI: lock enter (count only; no priority boost, no spinner)");
    cpu.install_hle(
        kLockLeave,
        [this](Cpu& c) {
            if (!kLifted && !enabled_) return false;
            const u32 count = c.mem_read32(kBlock + kLockCount) - 1u;
            c.mem_write32(kBlock + kLockCount, count);
            if (count == 0u && tick_pending_) interrupts_.raise(NativeInterrupts::Source::HostWork);
            return true;
        },
        "native CRI: lock leave (a deferred server tick runs once the count is 0)");
    // adxm_goto_mwidle_border: one Idle server call, then the callback, before returning.
    cpu.install_hle(
        kDrain,
        [this](Cpu& c) {
            if (!kLifted && (!enabled_ || !replaced_)) return false;
            if (!replaced_) {  // lifted: the drain's pointer is stored after the servers are created
                stop(c, "adxm_goto_mwidle_border before the CRI server threads were created (statically unreachable)");
                return true;
            }
            if (c.mem_read32(kBlock + kIdleExited) == 1u) return true;
            if (idle_active_) {  // the Idle thread was mid-pass: its stack is in use
                stop(c, "adxm_goto_mwidle_border while the Idle server's upcall runs");
                return true;
            }
            ++stats_.drains;
            c.mem_write32(kBlock + kDrainFlag, 1u);
            drain_phase_ = 1u;
            drain_lr_ = c.lr;
            drain_sp_ = c.gpr[1];
            c.mem_write32(kBlock + kIdleCount, c.mem_read32(kBlock + kIdleCount) + 1u);
            c.gpr[3] = 0u;
            // On the Idle thread's stack, where the original ran it (the caller waited for that thread).
            c.gpr[1] = ((stacks_[0] & ~7u) - 8u - 0x40u) & ~0xFu;
            c.mem_write32(c.gpr[1], 0u);
            c.lr = kDrainReturn;
            c.leave_to_dispatcher(kIdleServer, FallbackReason::Yield, kDrainReturn);
            return true;
        },
        "native CRI: adxm_goto_mwidle_border (one Idle server call, synchronously)");
    cpu.install_hle(
        kDrainReturn,
        [this](Cpu& c) {
            if (drain_phase_ == 1u) {  // the Idle server returned
                if (c.mem_read32(kBlock + kDrainFlag) == 1u) c.mem_write32(kBlock + kDrainFlag, 0u);
                const u32 callback = c.mem_read32(kUserCallback);
                if (callback != 0u) {
                    ++stats_.callbacks;
                    drain_phase_ = 2u;
                    c.gpr[3] = c.mem_read32(kUserCallback + 4u);
                    c.gpr[1] = ((stacks_[0] & ~7u) - 8u - 0x40u) & ~0xFu;
                    c.lr = kDrainReturn;
                    c.leave_to_dispatcher(callback, FallbackReason::Yield, kDrainReturn);
                    return true;
                }
            } else if (drain_phase_ != 2u) {
                stop(c, "the drain's continuation was entered with no drain in progress");
                return true;
            }
            drain_phase_ = 0u;
            idle_pending_ = false;  // the Idle thread suspended itself
            ++stats_.idle_passes;
            c.gpr[1] = drain_sp_;
            c.lr = drain_lr_;
            return true;
        },
        "native CRI: the drain's Idle server / callback returned");
    // Nothing else may touch the replaced threads: the thread API on one of them stops the run (it would read an
    // OSThread nobody initialised). Each is an install_hle on the API's own entry that serves only that case and
    // declines every other call (the original OS body then runs).
    const auto guard = [this](u32 api) {
        return [this, api](Cpu& c) {
            if (enabled_ && replaced_ && thread_index(c.gpr[3]) != ~0u) {
                char text[96];
                std::snprintf(text, sizeof(text), "thread API 0x%08X called on replaced CRI thread 0x%08X", api,
                              c.gpr[3]);
                stop(c, text);
                return true;
            }
            return false;
        };
    };
    if (kLifted) {
        // The four server bodies run only as threads, and no thread exists: their entries are passed to OSCreateThread (served above)
        // and appear nowhere else. A stop, never a guest body.
        for (const u32 entry : {kVsyncEntry, kSvtype4Entry, kIdleEntry, kSpinnerEntry}) {
            cpu.install_hle(entry, [this](Cpu& c) {
                stop(c, "a CRI server thread body was entered (the servers are native; statically unreachable)");
                return true;
            }, "native CRI: server thread entry (never entered)");
        }
    }
    cpu.install_hle(kSuspendThread, guard(kSuspendThread), "native CRI: OSSuspendThread guard (replaced CRI thread)");
    cpu.install_hle(kSetPriority, guard(kSetPriority), "native CRI: OSSetThreadPriority guard (replaced CRI thread)");
    cpu.install_hle(kGetPriority, guard(kGetPriority), "native CRI: OSGetThreadPriority guard (replaced CRI thread)");
}

void NativeCri::save_state(std::vector<u8>& out) const {
    state::put<u8>(out, replaced_ ? 1u : 0u);
    state::put<u8>(out, tick_pending_ ? 1u : 0u);
    state::put<u8>(out, idle_pending_ ? 1u : 0u);
    state::put<u8>(out, tick_active_ ? 1u : 0u);
    state::put<u8>(out, idle_active_ ? 1u : 0u);
    for (const bool r : resumed_) state::put<u8>(out, r ? 1u : 0u);
    for (const u32 stack : stacks_) state::put<u32>(out, stack);
    state::put<u32>(out, drain_phase_);
    state::put<u32>(out, drain_lr_);
    state::put<u32>(out, drain_sp_);
}

bool NativeCri::load_state(const u8*& cursor, const u8* end) {
    u8 replaced = 0, tick = 0, idle = 0, tick_active = 0, idle_active = 0;
    if (!state::get<u8>(cursor, end, replaced) || !state::get<u8>(cursor, end, tick) ||
        !state::get<u8>(cursor, end, idle) || !state::get<u8>(cursor, end, tick_active) ||
        !state::get<u8>(cursor, end, idle_active))
        return false;
    for (bool& r : resumed_) {
        u8 v = 0;
        if (!state::get<u8>(cursor, end, v)) return false;
        r = v != 0u;
    }
    for (u32& stack : stacks_)
        if (!state::get<u32>(cursor, end, stack)) return false;
    if (!state::get<u32>(cursor, end, drain_phase_) || !state::get<u32>(cursor, end, drain_lr_) ||
        !state::get<u32>(cursor, end, drain_sp_))
        return false;
    // A state whose CRI threads were replaced (never created as guest threads) needs the bridge: without it no CRI
    // server would run after the load (audio and streaming stop).
    if (replaced != 0u && !enabled_) {
        std::fprintf(stderr, "native CRI: the state was saved with SHADOW_NATIVE_CRI=1; it needs that setting\n");
        return false;
    }
    // The reverse: a state saved with the CRI threads alive as guest threads (guest memory is already restored: the idle
    // thread's OSThread state word is non-zero) cannot run on the bridge, whose servers are not those threads.
    if (replaced == 0u && enabled_ && (cpu_.mem_read32(kBlock + 0x30u + 0x2C8u) >> 16) != 0u) {
        std::fprintf(stderr, "native CRI: the state was saved with SHADOW_NATIVE_CRI=0 (CRI threads alive); it needs that setting\n");
        return false;
    }
    replaced_ = replaced != 0u;
    tick_pending_ = tick != 0u;
    idle_pending_ = idle != 0u;
    tick_active_ = tick_active != 0u;
    idle_active_ = idle_active != 0u;
    return true;
}

}  // namespace gekko
