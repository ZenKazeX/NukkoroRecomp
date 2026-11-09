#pragma once

// The OS's scheduler-disable count, owned natively (docs/agents/cri-threads.md "os.threads S1"; removal plan service os.threads).
//
// Original behaviour (main.dol, PAL GUPP8P): OSDisableScheduler 0x803784CC and OSEnableScheduler 0x8037850C are
//   enabled = OSDisableInterrupts(); n = *(s32*)(r13 + 0x5AE8); *(s32*)(r13 + 0x5AE8) = n +/- 1; OSRestoreInterrupts(enabled);
//   return n;
// The word (SDA 0x5AE8, 0x805F1FE8 with the game's r13) is the scheduler's own count: the reschedule code of the guest
// scheduler (SelectThread and its callers) reads it, so it stays a guest word for as long as that scheduler exists.
//
// Native implementation: the same read and write of that word, r3 = the previous value. The interrupt mask/restore pair
// disappears (a native owner is atomic: nothing is delivered inside it); what it guarded is kept: a deliverable interrupt
// is taken BEFORE the count changes, like the interrupt gate's owner hands back at its entry (native_interrupts.cpp
// interrupt_gate), and MSR[EE] ends as it began. Registers: r3 and the non-volatile registers, LR and MSR[EE] equal the
// original's; the volatile ones (r0, r4, r5, CR0) the original leaves holding the gate's scratch values are not kept.

#include "gekko/types.h"

#include <cstddef>
#include <functional>

namespace gekko {
class Cpu;
class NativeInterrupts;
}

namespace gekko::os_threads {

constexpr u32 kDisableScheduler = 0x803784CCu;
constexpr u32 kEnableScheduler = 0x8037850Cu;
constexpr u32 kSchedulerCountSda = 0x5AE8u;  // r13-relative
// Pure readers of thread state (S2): no interrupt gate, no write.
constexpr u32 kGetCurrentThread = 0x8037848Cu;    // r3 = *(u32*)0x800000E4 (the OS's current-thread word)
constexpr u32 kIsThreadTerminated = 0x80378498u;  // r3 = (u16)thread->state is 8 (moribund) or 0 (not a live thread)
constexpr u32 kGetThreadPriority = 0x80379574u;   // r3 = thread->base priority (+0x2D4)
constexpr u32 kInitThreadQueue = 0x8037847Cu;    // queue->head = queue->tail = 0 (two word stores; r3 = queue)
// The waits (S5, docs/agents/cri-threads.md "os.threads S5"): the one guest thread's wait is a native wait, not a switch.
constexpr u32 kYieldThread = 0x80378A08u;   // OSYieldThread(): with one thread, a no-op
constexpr u32 kSleepThread = 0x803792C4u;   // OSSleepThread(queue): the caller waits until OSWakeupThread(queue)
constexpr u32 kCreateThread = 0x80378A44u;   // OSCreateThread
constexpr u32 kExitThread = 0x80378C2Cu;     // OSExitThread
constexpr u32 kCancelThread = 0x80378D10u;   // OSCancelThread
constexpr u32 kResumeThread = 0x80378ECCu;   // OSResumeThread
constexpr u32 kSuspendThread = 0x80379154u;  // OSSuspendThread
constexpr u32 kSetThreadPriority = 0x803794B4u;  // OSSetThreadPriority
// The guest exception tails that end in OSLoadContext (the default exception report and the interrupt-handler tail the native
// delivery replaced). Entered only by the guest exception vectors, which this port serves natively; a fault if ever entered.
constexpr u32 kExceptionTail = 0x80371BE4u;
constexpr u32 kUnhandledExceptionTail = 0x80373590u;
constexpr u32 kUnhandledExceptionTailA = 0x80373684u;
constexpr u32 kUnhandledExceptionTailB = 0x80373708u;
constexpr u32 kGuestInterruptTail = 0x803765F4u;
// The game's Thread class (SonicteamUSA::System::Thread, vtable base 0x805608C8) and its two users, TExpandBG (vtable 0x8051E23C) and
// TBGRead (vtable 0x8051E2D0). An object of these classes comes only from their constructors (the constructors are the only code that
// materialises the vtable bases: tools/platform/closure_proof.py), and the constructors' only callers are the native loaders' owners
// (fn_8004C770, TBGReadManager::update), so the lifted build has none: the constructors, the destructors and the worker entry are
// faults if entered (docs/agents/cri-threads.md "Static closure of the thread API").
constexpr u32 kThreadClassCtor = 0x8035A15Cu, kThreadClassDtor = 0x8035A0C8u, kThreadClassEntry = 0x8035A01Cu;
constexpr u32 kExpandBgCtor = 0x80043E78u, kExpandBgDtor = 0x80043E10u;
constexpr u32 kReadBgCtor = 0x80045778u, kReadBgDtor = 0x80045714u;
constexpr u32 kReschedule = 0x803789D8u;     // __OSReschedule: SelectThread(0) when RunQueueHint is set
constexpr u32 kWakeupThread = 0x803793B0u;  // OSWakeupThread(queue): ends the wait on `queue`; nothing on any other queue
constexpr u32 kActiveThreadsHead = 0x800000DCu;  // __OSActiveThreadQueue head / tail: the threads that exist
constexpr u32 kActiveThreadsTail = 0x800000E0u;
constexpr u32 kCurrentThreadWord = 0x800000E4u;
constexpr u32 kThreadStateOffset = 0x2C8u;
constexpr u32 kThreadBasePriorityOffset = 0x2D4u;

// Must equal this owner's rows of extract/native-owners.txt (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kDisableScheduler, "OSDisableScheduler"},
                                                 {kEnableScheduler, "OSEnableScheduler"},
                                                 {kGetCurrentThread, "OSGetCurrentThread"},
                                                 {kIsThreadTerminated, "OSIsThreadTerminated"},
                                                 {kGetThreadPriority, "OSGetThreadPriority"},
                                                 {kInitThreadQueue, "OSInitThreadQueue"},
                                                 {kYieldThread, "OSYieldThread"},
                                                 {kSleepThread, "OSSleepThread"},
                                                 {kWakeupThread, "OSWakeupThread"}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

// One call each, entered at `entry`. Returns false when it handed control back to the dispatcher instead (a deliverable
// interrupt is taken first; the call restarts at `entry` afterwards).
bool serve_disable_scheduler(Cpu& cpu, u32 entry);
bool serve_enable_scheduler(Cpu& cpu, u32 entry);
// The three readers: pure functions of guest memory; they always complete (r3 only; the original also leaves scratch in r0).
void serve_get_current_thread(Cpu& cpu);
void serve_is_thread_terminated(Cpu& cpu);
void serve_get_thread_priority(Cpu& cpu);
void serve_init_thread_queue(Cpu& cpu);  // *(u32*)(r3 + 4) = 0; *(u32*)(r3) = 0

// The wait slot: runtime-owned state, no guest word (the scheduler's SetRun / UnsetRun, which the interrupt tail still runs,
// write the OSThread fields).
struct MainWait {
    bool active = false;  // the guest thread is inside OSSleepThread
    bool woken = false;   // OSWakeupThread(queue) named the queue while it waited
    u32 queue = 0;
};

// With exactly one guest thread (the active-thread list holds only the current thread) nothing can be switched to:
//  - OSYieldThread has nothing to yield to (a deliverable interrupt is taken first, like the original's OSDisableInterrupts).
//  - OSSleepThread(queue) waits on the wait slot: the game clock advances to the next event (Cpu::idle_wait) and every pending
//    source is processed natively (NativeInterrupts::run_pending_event: no exception, no rfi, no reschedule) until an
//    OSWakeupThread(queue) from one of those callbacks sets `woken`. MSR is never written. The callers measured (ShowRaster,
//    VIWaitForRetrace) sleep with interrupts disabled; a caller with MSR[EE] set is a loud stop, as is a second guest thread,
//    a nested sleep, a latched decrementer / external condition and a source the event runner cannot drive.
//  - OSWakeupThread(queue) sets `woken` when the slot waits on `queue`; nothing otherwise. A guest thread linked to the queue is
//    a loud stop.
// All return true when the call is served (a stop requested counts).
bool serve_yield_thread(Cpu& cpu, u32 entry);
// True when OSYieldThread would return at once without handing back: no deliverable interrupt and no second guest thread.
// A native owner that yields (NativeRtGcnFs's read: one yield per chunk and per transfer) continues inline then.
bool yield_is_noop(Cpu& cpu);
// `idle_pass` (may be empty): the scheduler's idle work between events - CRI's Idle pass; it returns true when it ran something
// (the wait then looks for pending events again) and is given the wait's wake flag as "a game thread became ready".
using IdlePass = std::function<bool(Cpu&, const bool* game_thread_ready)>;
bool serve_sleep_thread(Cpu& cpu, NativeInterrupts& interrupts, MainWait& wait, const IdlePass& idle_pass, u32 entry);
bool serve_wakeup_thread(Cpu& cpu, MainWait& wait, u32 entry);

void install(Cpu& cpu);
// The three waits above. After install(); the owners chain after hooks that exist at the address.
// True when no guest thread but the main one can exist: the native CRI servers and the three native loaders are on (their
// defaults; SHADOW_NATIVE_CRI=0 / GEKKO_NATIVE_BG_READ=0 / GEKKO_NATIVE_EXPAND_JOBS=0 / GEKKO_NATIVE_ONE=0 select the original guest
// threads). Decided once. The waits and the FPU policy below are the single-thread configuration's.
bool single_thread_configuration();

// The rest of the thread API in the single-thread configuration (lifted build; docs/agents/cri-threads.md "Thread lifecycle"):
// creating, ending, cancelling, resuming, suspending or re-prioritising a guest thread means a second thread exists or the
// main one leaves - ExecutionFault naming the function and its arguments. __OSReschedule with no ready thread (RunQueueHint 0)
// is the nothing the original does; with the hint set it is a fault too.
bool serve_thread_lifecycle_stop(Cpu& cpu, const char* name);
bool serve_reschedule(Cpu& cpu);

void install_waits(Cpu& cpu, NativeInterrupts& interrupts, IdlePass idle_pass);

}  // namespace gekko::os_threads
