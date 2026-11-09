#include "gekko/native_os_threads.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "gekko/cpu.h"
#include "gekko/native_interrupts.h"
#include "gekko/native_vi_wait.h"

namespace gekko::os_threads {
namespace {

bool serve(Cpu& c, u32 entry, s32 delta) {
    // The original's OSDisableInterrupts takes a deliverable interrupt before it writes MSR (hand back at the entry).
    if (c.interrupt_pending()) {
        c.leave_to_dispatcher(entry, FallbackReason::Yield, entry);
        return false;
    }
    const u32 address = c.gpr[13] + kSchedulerCountSda;
    const u32 previous = c.mem_read32(address);
    c.mem_write32(address, previous + static_cast<u32>(delta));
    c.gpr[3] = previous;
    return true;
}

// An owner installed on an address that already has a hook (native_cri's thread-API guard on OSGetThreadPriority,
// native_jobs' game-worker hook on OSIsThreadTerminated) runs AFTER it: the earlier hook serves its own cases (and returns
// true) or declines (false), and only a declined call reaches the owner. install_hle alone would replace the earlier hook.
void install_after_existing(Cpu& cpu, u32 address, Cpu::HleHandler owner, const char* name) {
    Cpu::HleHandler previous;
    for (const auto& entry : cpu.hle_entries())
        if (entry.address == address) previous = entry.handler;
    if (!previous) {
        cpu.install_hle(address, std::move(owner), name);
        return;
    }
    cpu.install_hle(address, [previous, owner](Cpu& c) { return previous(c) || owner(c); }, name);
}

bool only_current_thread(Cpu& c) {
    const u32 current = c.mem_read32(kCurrentThreadWord);
    return current != 0u && c.mem_read32(kActiveThreadsHead) == current && c.mem_read32(kActiveThreadsTail) == current;
}

bool stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "os.threads: " + why);
    return true;
}

}  // namespace

bool yield_is_noop(Cpu& c) {
    return !c.interrupt_pending() && only_current_thread(c);
}

bool serve_yield_thread(Cpu& c, u32 entry) {
    if (c.interrupt_pending()) {
        c.leave_to_dispatcher(entry, FallbackReason::Yield, entry);
        return true;
    }
    const bool none = c.mem_read32(kCurrentThreadWord) == 0u && c.mem_read32(kActiveThreadsHead) == 0u;  // before OSInit's thread
    if (!none && !only_current_thread(c)) return stop(c, "OSYieldThread with a second guest thread");
    return true;
}

bool serve_sleep_thread(Cpu& c, NativeInterrupts& interrupts, MainWait& wait, const IdlePass& idle_pass, u32 entry) {
    if (wait.active) return stop(c, "OSSleepThread inside a native wait");
    if (c.interrupt_pending()) {  // the original's OSDisableInterrupts takes it first; the wait starts afterwards
        c.leave_to_dispatcher(entry, FallbackReason::Yield, entry);
        return true;
    }
    const u32 queue = c.gpr[3];
    if (queue == 0u) return stop(c, "OSSleepThread on a null queue");
    if (!only_current_thread(c)) return stop(c, "OSSleepThread with a second guest thread");
    if ((c.msr() & kMsrEE) != 0u)
        return stop(c, "OSSleepThread with interrupts enabled (the native wait serves callers that disabled them)");
    wait = MainWait{true, false, queue};
    interrupts.set_wait_active(true);
    while (!wait.woken && !c.stopped() && !c.stop_requested()) {
        if (interrupts.run_pending_event(c)) continue;  // one source's native work; its callbacks may wake the queue
        if (idle_pass && idle_pass(c, &wait.woken)) continue;  // the Idle thread's turn (CRI), as at the idle spin
        if (c.decrementer_pending() || c.external_interrupt()) {
            (void)stop(c, c.decrementer_pending() ? "the decrementer is latched inside a native wait"
                                                  : "the external interrupt is latched inside a native wait");
            break;
        }
        c.idle_wait();  // the game clock runs to the next event and raises its source
    }
    interrupts.set_wait_active(false);
    wait = MainWait{};
    return true;
}

bool serve_thread_lifecycle_stop(Cpu& c, const char* name) {
    char text[200];
    std::snprintf(text, sizeof(text), "%s(0x%08X, 0x%08X) in the single-thread configuration (a guest thread would be created or the main one left)",
                  name, c.gpr[3], c.gpr[4]);
    return stop(c, text);
}

bool serve_reschedule(Cpu& c) {
    if (c.mem_read32(c.gpr[13] + 0x5AE4u) != 0u) return stop(c, "__OSReschedule with RunQueueHint set in the single-thread configuration");
    return true;
}

bool serve_wakeup_thread(Cpu& c, MainWait& wait, u32 entry) {
    if (c.interrupt_pending()) {
        c.leave_to_dispatcher(entry, FallbackReason::Yield, entry);
        return true;
    }
    const u32 queue = c.gpr[3];
    if (queue != 0u && c.mem_read32(queue) != 0u) return stop(c, "OSWakeupThread on a queue with a guest thread linked");
    if (wait.active && queue == wait.queue) wait.woken = true;
    return true;
}

bool serve_disable_scheduler(Cpu& cpu, u32 entry) { return serve(cpu, entry, 1); }
bool serve_enable_scheduler(Cpu& cpu, u32 entry) { return serve(cpu, entry, -1); }

void serve_get_current_thread(Cpu& c) { c.gpr[3] = c.mem_read32(kCurrentThreadWord); }

void serve_is_thread_terminated(Cpu& c) {
    const u32 state = c.mem_read16(c.gpr[3] + kThreadStateOffset);
    c.gpr[3] = (state == 8u || state == 0u) ? 1u : 0u;
}

void serve_get_thread_priority(Cpu& c) { c.gpr[3] = c.mem_read32(c.gpr[3] + kThreadBasePriorityOffset); }

void serve_init_thread_queue(Cpu& c) {
    c.mem_write32(c.gpr[3] + 4u, 0u);
    c.mem_write32(c.gpr[3], 0u);
}

void install(Cpu& cpu) {
    cpu.install_hle(kDisableScheduler, [](Cpu& c) {
        (void)serve_disable_scheduler(c, kDisableScheduler);
        return true;
    }, "OSDisableScheduler (native)");
    cpu.install_hle(kEnableScheduler, [](Cpu& c) {
        (void)serve_enable_scheduler(c, kEnableScheduler);
        return true;
    }, "OSEnableScheduler (native)");
    install_after_existing(cpu, kGetCurrentThread, [](Cpu& c) { serve_get_current_thread(c); return true; },
                           "OSGetCurrentThread (native)");
    install_after_existing(cpu, kIsThreadTerminated, [](Cpu& c) { serve_is_thread_terminated(c); return true; },
                           "OSIsThreadTerminated (native)");
    install_after_existing(cpu, kGetThreadPriority, [](Cpu& c) { serve_get_thread_priority(c); return true; },
                           "OSGetThreadPriority (native)");
    install_after_existing(cpu, kInitThreadQueue, [](Cpu& c) { serve_init_thread_queue(c); return true; },
                           "OSInitThreadQueue (native)");
}

bool single_thread_configuration_uncached() {
    // The waits assume the process has one guest thread: the native loaders and the native CRI servers (all default). A switch
    // set to 0 selects the original guest threads; those modes need the guest OSSleepThread / Wakeup / Yield and the thread
    // switch, i.e. the reference build (SHADOW_NATIVE_CRI=0, GEKKO_NATIVE_BG_READ=0, GEKKO_NATIVE_EXPAND_JOBS=0, GEKKO_NATIVE_ONE=0).
    for (const char* name : {"SHADOW_NATIVE_CRI", "GEKKO_NATIVE_BG_READ", "GEKKO_NATIVE_EXPAND_JOBS", "GEKKO_NATIVE_ONE"}) {
        const char* value = std::getenv(name);
        if (value != nullptr && value[0] == '0') return false;
    }
    return true;
}

bool single_thread_configuration() {
    static const bool value = single_thread_configuration_uncached();
    return value;
}

void install_waits(Cpu& cpu, NativeInterrupts& interrupts, IdlePass idle_pass) {
    if (!single_thread_configuration()) {
#if defined(SHADOW_RENDER_LIFTED)
        // The shipping (lifted) build prunes the guest bodies of these three functions (extract/native-owners-r2.txt): a mode that
        // creates guest threads has nothing to run them with.
        std::fprintf(stderr,
                     "os.threads: SHADOW_NATIVE_CRI=0 / GEKKO_NATIVE_BG_READ=0 / GEKKO_NATIVE_EXPAND_JOBS=0 / GEKKO_NATIVE_ONE=0 select "
                     "the original guest threads, which this build no longer contains; that mode needs the reference build.\n");
        std::exit(2);
#endif
        return;  // the reference build: the guest OSSleepThread / OSWakeupThread / OSYieldThread run
    }
    const auto wait = std::make_shared<MainWait>();
    const auto idle = std::make_shared<IdlePass>(std::move(idle_pass));
    install_after_existing(cpu, kYieldThread, [](Cpu& c) { return serve_yield_thread(c, kYieldThread); }, "OSYieldThread (native)");
    install_after_existing(cpu, kSleepThread,
                           [wait, idle, &interrupts](Cpu& c) { return serve_sleep_thread(c, interrupts, *wait, *idle, kSleepThread); },
                           "OSSleepThread (native)");
    struct Lifecycle { u32 address; const char* name; };
    for (const Lifecycle& row : {Lifecycle{kCreateThread, "OSCreateThread"}, Lifecycle{kExitThread, "OSExitThread"},
                                 Lifecycle{kCancelThread, "OSCancelThread"}, Lifecycle{kResumeThread, "OSResumeThread"},
                                 Lifecycle{kSuspendThread, "OSSuspendThread"}, Lifecycle{kSetThreadPriority, "OSSetThreadPriority"}}) {
        const char* name = row.name;
        install_after_existing(cpu, row.address, [name](Cpu& c) { return serve_thread_lifecycle_stop(c, name); }, name);
    }
    struct Tail { u32 address; const char* name; };
    for (const Tail& row : {Tail{kExceptionTail, "the guest exception tail 0x80371BE4"},
                            Tail{kUnhandledExceptionTail, "the guest unhandled-exception report 0x80373590"},
                            Tail{kUnhandledExceptionTailA, "the guest exception report helper 0x80373684"},
                            Tail{kUnhandledExceptionTailB, "the guest exception report helper 0x80373708"},
                            Tail{kGuestInterruptTail, "the guest interrupt-handler tail 0x803765F4"}}) {
        const char* name = row.name;
        install_after_existing(cpu, row.address, [name](Cpu& c) { return serve_thread_lifecycle_stop(c, name); }, name);
    }
    for (const Tail& row : {Tail{kThreadClassCtor, "Thread::Thread 0x8035A15C"}, Tail{kThreadClassDtor, "Thread::~Thread 0x8035A0C8"},
                            Tail{kThreadClassEntry, "the Thread worker entry 0x8035A01C"}, Tail{kExpandBgCtor, "TExpandBG::TExpandBG 0x80043E78"},
                            Tail{kExpandBgDtor, "TExpandBG::~TExpandBG 0x80043E10"}, Tail{kReadBgCtor, "TBGRead::TBGRead 0x80045778"},
                            Tail{kReadBgDtor, "TBGRead::~TBGRead 0x80045714"}}) {
        const char* name = row.name;
        install_after_existing(cpu, row.address, [name](Cpu& c) { return serve_thread_lifecycle_stop(c, name); }, name);
    }
    install_after_existing(cpu, kReschedule, [](Cpu& c) { return serve_reschedule(c); }, "__OSReschedule (native)");
    install_after_existing(cpu, kWakeupThread, [wait](Cpu& c) { return serve_wakeup_thread(c, *wait, kWakeupThread); },
                           "OSWakeupThread (native)");
    vi_wait::install(cpu);  // VIWaitForRetrace and its two resume rows: the same single-thread wait (docs/agents/vi.md Step 17)
}

}  // namespace gekko::os_threads
