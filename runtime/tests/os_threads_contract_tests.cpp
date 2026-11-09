// Original-DOL contract for the owned scheduler-disable count (OSDisableScheduler 0x803784CC, OSEnableScheduler 0x8037850C)
// and the three pure thread readers (OSGetCurrentThread 0x8037848C, OSIsThreadTerminated 0x80378498, OSGetThreadPriority 0x80379574).
// liveness-roots: reference-interpreter-only
// No generated reference body: every SDK instruction (the two functions and the interrupt gate they call) comes from the
// pinned DOL, run by the reference interpreter on a bare machine. The oracle runs the original for each input; the owner
// (serve_*) runs the same input on a second, identically prepared machine. Compared after the call: r3, the non-volatile
// registers r1 / r13-r31, LR, MSR[EE], the count word and its neighbours (the run-queue bits and the reschedule hint
// live next to it and must stay untouched). Volatile scratch (r0, r4, r5, CR0) is not part of the contract.
//
// Negative controls: three deliberately wrong owners (count unchanged, delta of the wrong sign, MSR[EE] left cleared)
// must each be caught by the same comparison.
// Readers: r3, the non-volatile registers, LR and the whole OSThread-sized window (nothing may be written) are compared over
// thread states / priorities / thread addresses; negative controls: state==8 only, a zero current thread, the wrong priority field.
// Not covered here: the interrupt-pending hand-back (it mirrors the interrupt gate's owner, tested by
// os_interrupts_contract_tests; the Westopolis / Final Haunt / Lethal Highway replays exercise it with real interrupts).
//
// usage: os_threads_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_interrupts.h"
#include "gekko/native_os_threads.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
using namespace gekko;
using namespace gekko::os_threads;
constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kEnableBit = 0x8000u;
constexpr u32 kWindow = 0x5AD8u, kWindowWords = 12u;  // r13 + 0x5AD8 .. +0x5B08: the scheduler words

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    explicit Machine(const std::vector<u8>& dol) {
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
    }
    void prepare(u32 count, bool interrupts_enabled, u32 salt) {
        cpu.reset();
        cpu.set_msr(interrupts_enabled ? (cpu.msr() | kEnableBit) : (cpu.msr() & ~kEnableBit));
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13;
        for (u32 r = 14; r < 32; ++r) cpu.gpr[r] = 0xA5000000u + salt * 0x100u + r;
        cpu.lr = kReturn;
        for (u32 i = 0; i < kWindowWords; ++i) memory.write32(kR13 + kWindow + 4 * i, 0x5EED0000u + i);
        memory.write32(kR13 + kSchedulerCountSda, count);
    }
    void run_original(u32 entry) {
        cpu.pc = entry; cpu.npc = entry + 4;
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 100000) cpu.run_steps(1);
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack, "original call failed return/stack contract");
    }
};

struct Result {
    std::array<u32, 21> regs{};  // r3, r1, r13..r31
    u32 lr = 0, ee = 0;
    std::array<u32, kWindowWords> window{};
    bool operator==(const Result& o) const {
        return regs == o.regs && lr == o.lr && ee == o.ee && window == o.window;
    }
};

Result observe(Machine& m) {
    Result r;
    std::size_t at = 0;
    r.regs[at++] = m.cpu.gpr[3];
    r.regs[at++] = m.cpu.gpr[1];
    for (u32 g = 13; g < 32; ++g) r.regs[at++] = m.cpu.gpr[g];
    r.lr = m.cpu.lr;
    r.ee = m.cpu.msr() & kEnableBit;
    for (u32 i = 0; i < kWindowWords; ++i) r.window[i] = m.memory.read32(kR13 + kWindow + 4 * i);
    return r;
}

using Serve = std::function<bool(Cpu&, u32)>;

// ---- the readers --------------------------------------------------------------------------------------------------
constexpr u32 kThreadA = 0x80600000u, kThreadB = 0x80601000u, kThreadWindow = 0x300u;
using ReaderServe = std::function<void(Cpu&)>;

void prepare_reader(Machine& m, u32 thread, u32 state, u32 priority, u32 current, u32 salt) {
    m.cpu.reset();
    m.cpu.gpr[1] = kStack; m.cpu.gpr[2] = kR2; m.cpu.gpr[13] = kR13;
    for (u32 r = 14; r < 32; ++r) m.cpu.gpr[r] = 0xA5000000u + salt * 0x100u + r;
    m.cpu.lr = kReturn;
    for (u32 a : {kThreadA, kThreadB})
        for (u32 i = 0; i < kThreadWindow; i += 4) m.memory.write32(a + i, 0x7E570000u + (i ^ salt));
    m.memory.write16(thread + kThreadStateOffset, static_cast<u16>(state));
    m.memory.write16(thread + kThreadStateOffset + 2, 0xBEEF);
    m.memory.write32(thread + 0x2D0u, priority ^ 0x55u);   // the effective-priority word next to the base one
    m.memory.write32(thread + kThreadBasePriorityOffset, priority);
    m.memory.write32(kCurrentThreadWord, current);
    m.cpu.gpr[3] = thread;
}

Result observe_reader(Machine& m) {
    Result r = observe(m);
    for (u32 a : {kThreadA, kThreadB})
        for (u32 i = 0; i < kThreadWindow; i += 4) r.window[(i / 4 + (a == kThreadB ? 7 : 0)) % kWindowWords] ^= m.memory.read32(a + i) * (i + 1);
    r.window[0] ^= m.memory.read32(kCurrentThreadWord);
    return r;
}

void run_readers(Machine& original, Machine& native, unsigned& runs, unsigned (&mismatches)[3]) {
    struct Reader { u32 entry; void (*serve)(Cpu&); const char* name; };
    const Reader readers[] = {{kGetCurrentThread, serve_get_current_thread, "OSGetCurrentThread"},
                              {kIsThreadTerminated, serve_is_thread_terminated, "OSIsThreadTerminated"},
                              {kGetThreadPriority, serve_get_thread_priority, "OSGetThreadPriority"},
                              {kInitThreadQueue, serve_init_thread_queue, "OSInitThreadQueue"}};
    const u32 states[] = {0u, 1u, 2u, 3u, 4u, 5u, 8u, 9u, 0x0100u, 0x8000u, 0xFFFFu, 0x0008u};
    const u32 priorities[] = {0u, 1u, 15u, 16u, 31u, 0x80000000u, 0xFFFFFFFFu};
    const u32 currents[] = {0u, kThreadA, kThreadB, 0xDEADBEEFu};
    for (std::size_t ri = 0; ri < 4; ++ri) {
        const Reader& rd = readers[ri];
        const ReaderServe variants[4] = {
            ReaderServe(rd.serve),
            // wrong 1: IsTerminated treats only state 8 as terminated; GetCurrent returns the thread being asked about;
            //          GetPriority returns the effective-priority word
            ReaderServe([ri](Cpu& c) {
                if (ri == 0) c.gpr[3] = c.gpr[3];
                else if (ri == 3) c.mem_write32(c.gpr[3], 0u);  // only the head is cleared
                else if (ri == 1) c.gpr[3] = c.mem_read16(c.gpr[3] + kThreadStateOffset) == 8u ? 1u : 0u;
                else c.gpr[3] = c.mem_read32(c.gpr[3] + 0x2D0u);
            }),
            // wrong 2: the result is inverted / zeroed
            ReaderServe([ri, rd](Cpu& c) {
                const u32 keep = c.gpr[3];
                rd.serve(c);
                if (ri == 3) { c.mem_write32(c.gpr[3] + 4u, 1u); return; }
                c.gpr[3] = ri == 0 ? 0u : (ri == 1 ? (c.gpr[3] ^ 1u) : (keep == 0 ? c.gpr[3] : c.gpr[3] + 1u));
            }),
            // wrong 3: a non-volatile register is clobbered
            ReaderServe([rd](Cpu& c) { rd.serve(c); c.gpr[31] ^= 1u; })};
        for (u32 state : states)
            for (u32 priority : priorities)
                for (u32 current : currents)
                    for (u32 thread : {kThreadA, kThreadB})
                        for (u32 salt = 0; salt < 2; ++salt) {
                            prepare_reader(original, thread, state, priority, current, salt);
                            original.run_original(rd.entry);
                            const Result expected = observe_reader(original);
                            for (int v = 0; v < 4; ++v) {
                                prepare_reader(native, thread, state, priority, current, salt);
                                variants[v](native.cpu);
                                const bool same = observe_reader(native) == expected;
                                if (v == 0) {
                                    if (!same) std::fprintf(stderr, "differs: %s state %04X prio %08X cur %08X thread %08X\n", rd.name, state, priority, current, thread);
                                    require(same, rd.name);
                                } else if (!same) {
                                    ++mismatches[v - 1];
                                }
                            }
                            ++runs;
                        }
    }
}

}  // namespace

// Hook chaining (the defect of c6496a1): install_hle replaces an earlier hook at the same address. With an earlier hook
// installed first (native_jobs' IsTerminated, native_cri's GetPriority guard, anything else), os_threads::install must
// run it first: a call it serves never reaches the owner, a call it declines does.
unsigned run_chain(const std::vector<u8>& dol) {
    struct Row { u32 entry; const char* name; };
    const Row rows[] = {{kGetCurrentThread, "OSGetCurrentThread"}, {kIsThreadTerminated, "OSIsThreadTerminated"},
                        {kGetThreadPriority, "OSGetThreadPriority"}, {kInitThreadQueue, "OSInitThreadQueue"}};
    unsigned checks = 0;
    for (const Row& row : rows) {
        for (bool earlier_serves : {true, false}) {
            Machine m(dol);
            unsigned earlier_calls = 0;
            bool serve = earlier_serves;
            m.cpu.install_hle(row.entry, [&](Cpu& c) {
                ++earlier_calls;
                if (!serve) return false;
                c.gpr[3] = 0x1234u;
                return true;
            }, "earlier hook");
            install(m.cpu);
            Cpu::HleHandler handler;
            unsigned installed = 0;
            for (const auto& e : m.cpu.hle_entries())
                if (e.address == row.entry) { handler = e.handler; ++installed; }
            require(installed == 1 && handler, row.name);
            prepare_reader(m, kThreadA, 8u, 5u, kThreadA, 1u);
            m.memory.write32(kThreadA, 0x77777777u);
            m.memory.write32(kThreadA + 4u, 0x77777777u);
            require(handler(m.cpu), row.name);
            require(earlier_calls == 1, "the earlier hook was not consulted first");
            if (earlier_serves) {
                require(m.cpu.gpr[3] == 0x1234u, "the owner replaced the call the earlier hook served");
                require(m.memory.read32(kThreadA) == 0x77777777u && m.memory.read32(kThreadA + 4u) == 0x77777777u,
                        "the owner ran after the earlier hook served the call");
            } else {
                switch (row.entry) {
                    case kGetCurrentThread: require(m.cpu.gpr[3] == kThreadA, "owner not reached"); break;
                    case kIsThreadTerminated: require(m.cpu.gpr[3] == 1u, "owner not reached"); break;
                    case kGetThreadPriority: require(m.cpu.gpr[3] == 5u, "owner not reached"); break;
                    default: require(m.memory.read32(kThreadA) == 0u && m.memory.read32(kThreadA + 4u) == 0u, "owner not reached"); break;
                }
            }
            ++checks;
        }
    }
    return checks;
}

// The waits (S5). A world with one guest thread: the OS words the owners read, then each owner's contract.
constexpr u32 kQueue = 0x80602000u, kOtherQueue = 0x80602010u;
void one_thread_world(Machine& m, u32 msr_ee) {
    m.cpu.reset();
    m.cpu.set_msr(msr_ee ? (m.cpu.msr() | kEnableBit) : (m.cpu.msr() & ~kEnableBit));
    m.cpu.gpr[1] = kStack; m.cpu.gpr[2] = kR2; m.cpu.gpr[13] = kR13;
    m.cpu.lr = kReturn;
    for (u32 a : {kQueue, kOtherQueue}) { m.memory.write32(a, 0u); m.memory.write32(a + 4u, 0u); }
    for (u32 i = 0; i < kThreadWindow; i += 4) m.memory.write32(kThreadA + i, 0x11110000u + i);
    m.memory.write32(kCurrentThreadWord, kThreadA);
    m.memory.write32(kActiveThreadsHead, kThreadA);
    m.memory.write32(kActiveThreadsTail, kThreadA);
}

unsigned run_waits(const std::vector<u8>& dol) {
    unsigned checks = 0;
    Machine m(dol);
    NativeInterrupts ni(m.cpu);
    MainWait wait;
    const auto stopped = [&] { return m.cpu.stopped(); };
    const auto stop_text = [&] { return m.cpu.stop().detail; };
    // Wakeup: names the waited queue -> woken; another queue or nobody waiting -> nothing; a linked guest thread -> stop.
    one_thread_world(m, 0);
    wait = MainWait{true, false, kQueue};
    m.cpu.gpr[3] = kOtherQueue;
    require(serve_wakeup_thread(m.cpu, wait, kWakeupThread) && !stopped() && !wait.woken, "wakeup of another queue");
    m.cpu.gpr[3] = kQueue;
    require(serve_wakeup_thread(m.cpu, wait, kWakeupThread) && !stopped() && wait.woken, "wakeup"); ++checks;
    wait = MainWait{};
    require(serve_wakeup_thread(m.cpu, wait, kWakeupThread) && !wait.woken, "wakeup with nobody waiting"); ++checks;
    one_thread_world(m, 0);
    m.memory.write32(kQueue, kThreadB);
    m.cpu.gpr[3] = kQueue;
    require(serve_wakeup_thread(m.cpu, wait, kWakeupThread) && stopped(), "a linked guest thread must stop the machine"); ++checks;
    // Wakeup / Yield take a deliverable interrupt first (hand back at the entry, nothing changed).
    one_thread_world(m, 1);
    wait = MainWait{true, false, kQueue};
    m.cpu.gpr[3] = kQueue;
    m.cpu.set_native_interrupt(true);
    require(serve_wakeup_thread(m.cpu, wait, kWakeupThread) && m.cpu.pc == kWakeupThread && !wait.woken, "wakeup did not hand back"); ++checks;
    one_thread_world(m, 1);
    m.cpu.set_native_interrupt(true);
    require(serve_yield_thread(m.cpu, kYieldThread) && m.cpu.pc == kYieldThread, "yield did not hand back"); ++checks;
    // Yield: nothing to yield to -> no change; a second thread -> stop.
    one_thread_world(m, 1);
    require(serve_yield_thread(m.cpu, kYieldThread) && !stopped(), "yield"); ++checks;
    one_thread_world(m, 1);
    m.memory.write32(kActiveThreadsTail, kThreadB);
    require(serve_yield_thread(m.cpu, kYieldThread) && stopped(), "a second guest thread must stop yield"); ++checks;

    // Sleep: the idle handler plays "the clock reached the retrace" (raises the VI source); the VI handler is a step machine
    // that calls a guest function (OSGetCurrentThread, original words) and then wakes the queue; the sleep ends. MSR, r1,
    // the registers the callee preserves and every OSThread word are untouched.
    const u32 unmask = 0x80000000u >> 24;
    const auto set_up_vi = [&](unsigned& steps, unsigned& callee_results_ok, bool wake) {
        ni.reset();
        m.memory.write32(0x800000C4u, ~0u);  // OS global mask: all masked
        m.memory.write32(0x800000C8u, 0u);
        (void)ni.unmask_os_interrupts(m.cpu, unmask);
        ni.set_handler(NativeInterrupts::Source::Vi, [&, wake](Cpu& c, u32 frame, u32 step) {
            ++steps;
            require((c.msr() & kEnableBit) == 0u, "the event ran with MSR[EE] set");
            if (step == 0u) {
                ni.call_guest(c, frame, kGetCurrentThread, 0u, 0u);
                return true;
            }
            if (c.gpr[3] == kThreadA) ++callee_results_ok;
            if (wake) {
                c.gpr[3] = kQueue;
                (void)serve_wakeup_thread(c, wait, kWakeupThread);
            }
            return false;
        });
    };
    one_thread_world(m, 0);
    unsigned steps = 0, ok_results = 0, idles = 0;
    set_up_vi(steps, ok_results, true);
    wait = MainWait{};
    m.cpu.set_idle_handler([&](Cpu&) { ++idles; ni.raise(NativeInterrupts::Source::Vi); });
    m.cpu.gpr[3] = kQueue;
    m.cpu.gpr[20] = 0x20202020u;
    const u32 msr_before = m.cpu.msr();
    std::array<u32, kThreadWindow / 4> before{};
    for (u32 i = 0; i < before.size(); ++i) before[i] = m.memory.read32(kThreadA + 4u * i);
    require(serve_sleep_thread(m.cpu, ni, wait, IdlePass(), kSleepThread), "sleep");
    if (stopped()) std::fprintf(stderr, "stopped: %s\n", stop_text().c_str());
    require(!stopped() && idles == 1 && steps == 2 && ok_results == 1, "sleep did not run the event's callback and end");
    require(m.cpu.msr() == msr_before && m.cpu.gpr[1] == kStack && m.cpu.gpr[20] == 0x20202020u && !wait.active && !wait.woken,
            "sleep changed the MSR, r1 or a preserved register");
    for (u32 i = 0; i < before.size(); ++i) require(m.memory.read32(kThreadA + 4u * i) == before[i], "sleep wrote an OSThread word");
    ++checks;
    // An event that wakes nobody: the wait goes on to the next event (the second one wakes).
    one_thread_world(m, 0);
    steps = ok_results = idles = 0;
    set_up_vi(steps, ok_results, false);
    wait = MainWait{};
    m.cpu.set_idle_handler([&](Cpu& c) {
        ++idles;
        ni.raise(NativeInterrupts::Source::Vi);
        if (idles == 3) {
            c.gpr[3] = kQueue;  // an external wake between events: the slot is woken by the third event's work
            wait.woken = true;
        }
    });
    m.cpu.gpr[3] = kQueue;
    require(serve_sleep_thread(m.cpu, ni, wait, IdlePass(), kSleepThread) && !stopped() && idles == 3 && steps == 4, "sleep over events that wake nobody"); ++checks;
    // The loud stops.
    one_thread_world(m, 1);
    m.cpu.gpr[3] = kQueue;
    wait = MainWait{};
    require(serve_sleep_thread(m.cpu, ni, wait, IdlePass(), kSleepThread) && stopped() && stop_text().find("interrupts enabled") != std::string::npos,
            "sleep with interrupts enabled must stop"); ++checks;
    one_thread_world(m, 0);
    m.memory.write32(kActiveThreadsHead, kThreadB);
    m.cpu.gpr[3] = kQueue;
    require(serve_sleep_thread(m.cpu, ni, wait, IdlePass(), kSleepThread) && stopped(), "sleep with a second thread must stop"); ++checks;
    one_thread_world(m, 0);
    m.cpu.gpr[3] = kQueue;
    wait = MainWait{true, false, kOtherQueue};
    require(serve_sleep_thread(m.cpu, ni, wait, IdlePass(), kSleepThread) && stopped(), "a nested sleep must stop"); ++checks;
    one_thread_world(m, 0);
    ni.reset();
    ni.raise(NativeInterrupts::Source::HostWork);
    m.cpu.gpr[3] = kQueue;
    wait = MainWait{};
    require(serve_sleep_thread(m.cpu, ni, wait, IdlePass(), kSleepThread) && stopped() && stop_text().find("host work") != std::string::npos,
            "a latched HostWork must stop the wait, naming it"); ++checks;
    one_thread_world(m, 0);
    ni.reset();
    m.cpu.set_external_interrupt(true);
    m.cpu.gpr[3] = kQueue;
    wait = MainWait{};
    require(serve_sleep_thread(m.cpu, ni, wait, IdlePass(), kSleepThread) && stopped() && stop_text().find("external") != std::string::npos,
            "a latched external interrupt must stop the wait"); ++checks;
    // A handler that asks for an upcall.
    one_thread_world(m, 0);
    steps = ok_results = idles = 0;
    set_up_vi(steps, ok_results, false);
    ni.set_handler(NativeInterrupts::Source::Vi, [&](Cpu&, u32, u32) { ni.request_upcall(7u, 0u); return false; });
    m.cpu.set_idle_handler([&](Cpu&) { ni.raise(NativeInterrupts::Source::Vi); });
    m.cpu.gpr[3] = kQueue;
    wait = MainWait{};
    require(serve_sleep_thread(m.cpu, ni, wait, IdlePass(), kSleepThread) && stopped() && stop_text().find("upcall") != std::string::npos,
            "an upcall inside a wait must stop"); ++checks;
    return checks;
}

// The preemption yield of the delivery's end (SelectThread(1) with one thread) is skipped only when it is the identity.
unsigned run_yield_identity(const std::vector<u8>& dol) {
    unsigned checks = 0;
    Machine m(dol);
    NativeInterrupts ni(m.cpu);
    const u32 sda = kR13;
    const auto world = [&] {
        one_thread_world(m, 0);
        m.memory.write32(0x800000D4u, kThreadA);                // current context = the thread's own
        m.memory.write16(kThreadA + 0x2C8u, 2u);                // running
        m.memory.write32(kThreadA + 0x2D0u, 16u);               // priority
        for (u32 off : {0x2DCu, 0x2E0u, 0x2E4u}) m.memory.write32(kThreadA + off, 0u);
        m.memory.write16(kThreadA + 0x1A2u, 2u);                // saved by an exception
        for (u32 i = 0; i < 64u; i += 4u) m.memory.write32(0x80587038u + i + 0x40u, 0u);
        m.memory.write32(sda + 0x5AE0u, 0u);                    // ready bits
        m.memory.write32(sda + 0x5AE4u, 0u);                    // hint
        m.memory.write32(sda + 0x5AE8u, 0u);                    // scheduler disable count
        m.memory.write32(sda + 0x2710u, 0x80378320u);           // the empty switch callback
        m.cpu.gpr[13] = sda;
    };
    world();
    require(ni.yield_is_identity(m.cpu), "one running thread is the identity"); ++checks;
    // Negative controls: every condition the original would act on makes it not the identity.
    struct Mutation { const char* name; u32 address; u32 value; bool half; };
    const Mutation mutations[] = {
        {"a second thread in the active list", kActiveThreadsTail, kThreadB, false},
        {"the thread is ready, not running", kThreadA + 0x2C8u, 1u, true},
        {"the thread is on a queue", kThreadA + 0x2DCu, 0x80587038u, false},
        {"a linked successor", kThreadA + 0x2E0u, kThreadB, false},
        {"a ready bit", sda + 0x5AE0u, 0x00008000u, false},
        {"a queued entry at its priority", 0x80587038u + 8u * 16u, kThreadB, false},
        {"another switch callback", sda + 0x2710u, 0x80400000u, false},
        {"the context was not saved by an exception", kThreadA + 0x1A2u, 0u, true},
    };
    for (const Mutation& mu : mutations) {
        world();
        if (mu.half) m.memory.write16(mu.address, static_cast<u16>(mu.value)); else m.memory.write32(mu.address, mu.value);
        require(!ni.yield_is_identity(m.cpu), mu.name); ++checks;
    }
    // SelectThread returns at once (no switch) with switches disabled or another context current: also the identity.
    world();
    m.memory.write32(sda + 0x5AE8u, 1u);
    m.memory.write32(kThreadA + 0x2C8u, 1u);
    require(ni.yield_is_identity(m.cpu), "switches disabled"); ++checks;
    world();
    m.memory.write32(0x800000D4u, 0x80600800u);
    m.memory.write32(kActiveThreadsTail, kThreadB);
    require(ni.yield_is_identity(m.cpu), "another context current"); ++checks;
    return checks;
}

// OSLoadContext in host code: registers, SRR0/SRR1, the EXC flag and the OSDisableInterrupts clamp.
unsigned run_restore_context(const std::vector<u8>& dol) {
    unsigned checks = 0;
    Machine m(dol);
    NativeInterrupts ni(m.cpu);
    constexpr u32 ctx = 0x80600400u;
    const auto world = [&](u16 state, u32 srr0) {
        one_thread_world(m, 0);
        for (u32 i = 0; i < 0x2C8u; i += 4u) m.memory.write32(ctx + i, 0xA0000000u + i);
        m.memory.write16(ctx + 0x1A2u, state);
        m.memory.write32(ctx + 0x198u, srr0);
        m.memory.write32(ctx + 0x19Cu, 0x0000B032u);
        for (u32 r = 0; r < 32u; ++r) m.cpu.gpr[r] = 0x55555555u;
    };
    world(2u, 0x80100000u);
    require(ni.restore_interrupted_context(m.cpu, ctx), "restore of an exception-saved context"); ++checks;
    require(m.cpu.gpr[0] == 0xA0000000u && m.cpu.gpr[1] == 0xA0000004u && m.cpu.gpr[2] == 0xA0000008u, "r0-r2"); ++checks;
    require(m.cpu.gpr[3] == 0xA000000Cu && m.cpu.gpr[4] == 0xA0000010u && m.cpu.gpr[5] == 0xA0000014u && m.cpu.gpr[31] == 0xA0000014u + 4u * 26u, "r3-r5, r31"); ++checks;
    require(m.cpu.cr == 0xA0000080u && m.cpu.lr == 0xA0000084u && m.cpu.ctr == 0xA0000088u && m.cpu.xer == 0xA000008Cu, "cr lr ctr xer"); ++checks;
    require(m.cpu.spr[SPR_GQR0 + 1] == 0xA00001A8u && m.cpu.spr[SPR_GQR0 + 7] == 0xA00001C0u, "gqr1-7"); ++checks;
    require(m.cpu.pc == 0x80100000u, "resumes at SRR0"); ++checks;
    require((m.memory.read16(ctx + 0x1A2u) & 2u) == 0u, "the EXC flag is cleared"); ++checks;
    // A context interrupted inside OSDisableInterrupts resumes at its start; one outside the window does not move.
    world(2u, 0x80376120u);
    require(ni.restore_interrupted_context(m.cpu, ctx) && m.cpu.pc == 0x8037611Cu && m.memory.read32(ctx + 0x198u) == 0x8037611Cu, "the OSDisableInterrupts clamp"); ++checks;
    world(2u, 0x80376128u);
    require(ni.restore_interrupted_context(m.cpu, ctx) && m.cpu.pc == 0x80376128u, "no clamp at the tail"); ++checks;
    // Against the original: the guest OSLoadContext (0x80372C68) and the host restore leave the same registers, the same SRR0 /
    // SRR1, the same MSR and the same context words (r5-r12 included for an exception-saved context). A planted wrong host
    // value (the control) must be seen.
    struct Snapshot { std::array<u32, 32> gpr; u32 cr, lr, ctr, xer, msr, pc, srr0, srr1; std::array<u32, 8> gqr; std::array<u32, 0x2C8 / 4> words; };
    const auto snap = [&] {
        Snapshot s{};
        for (u32 r = 0; r < 32u; ++r) s.gpr[r] = m.cpu.gpr[r];
        s.cr = m.cpu.cr; s.lr = m.cpu.lr; s.ctr = m.cpu.ctr; s.xer = m.cpu.xer; s.msr = m.cpu.msr(); s.pc = m.cpu.pc;
        s.srr0 = m.cpu.spr[SPR_SRR0]; s.srr1 = m.cpu.spr[SPR_SRR1];
        for (u32 g = 0; g < 8u; ++g) s.gqr[g] = m.cpu.spr[SPR_GQR0 + g];
        for (u32 i = 0; i < s.words.size(); ++i) s.words[i] = m.memory.read32(ctx + 4u * i);
        return s;
    };
    const auto same = [](const Snapshot& x, const Snapshot& y) {
        return x.gpr == y.gpr && x.cr == y.cr && x.lr == y.lr && x.ctr == y.ctr && x.xer == y.xer && x.msr == y.msr && x.pc == y.pc &&
               x.srr0 == y.srr0 && x.srr1 == y.srr1 && x.gqr == y.gqr && x.words == y.words;
    };
    for (u32 srr0 : {0x80100000u, 0x80376120u, 0x8037611Cu, 0x80376128u, 0x8037611Du}) {
        world(2u, srr0);
        m.cpu.set_msr(m.cpu.msr() | kEnableBit);
        m.cpu.gpr[3] = ctx;
        m.cpu.pc = 0x80372C68u; m.cpu.npc = m.cpu.pc + 4u;
        unsigned steps = 0;
        const u32 want = srr0 > 0x8037611Cu && srr0 < 0x80376128u ? 0x8037611Cu : srr0;
        while (m.cpu.pc != want && !m.cpu.stopped() && steps++ < 1000) m.cpu.run_steps(1);
        require(m.cpu.pc == want && !m.cpu.stopped(), "the original OSLoadContext did not reach SRR0");
        const Snapshot original = snap();
        world(2u, srr0);
        m.cpu.set_msr(m.cpu.msr() | kEnableBit);
        require(ni.restore_interrupted_context(m.cpu, ctx), "host restore");
        Snapshot host = snap();
        // the original left r5-r12 from the context too; r3/r4 last: equal in both.
        require(same(original, host), "the host restore differs from the original OSLoadContext"); ++checks;
        host.gpr[7] ^= 1u;
        require(!same(original, host), "the comparison cannot see a wrong register"); ++checks;
    }
    // Negative control: a context without the EXC flag is not served (the guest function restores it).
    world(0u, 0x80100000u);
    require(!ni.restore_interrupted_context(m.cpu, ctx) && m.cpu.gpr[0] == 0x55555555u && m.memory.read16(ctx + 0x1A2u) == 0u, "a context without EXC must not be served"); ++checks;
    return checks;
}

// The thread API in the single-thread configuration: lifecycle calls are faults naming the function; __OSReschedule is a no-op
// with RunQueueHint clear and a fault with it set.
unsigned run_lifecycle(const std::vector<u8>& dol) {
    unsigned checks = 0;
    Machine m(dol);
    one_thread_world(m, 0);
    m.cpu.gpr[3] = 0x80600000u; m.cpu.gpr[4] = 7u;
    require(serve_thread_lifecycle_stop(m.cpu, "OSCreateThread") && m.cpu.stopped(), "a lifecycle call must stop"); ++checks;
    require(m.cpu.stop().detail.find("OSCreateThread(0x80600000, 0x00000007)") != std::string::npos, "the stop names the function and arguments"); ++checks;
    one_thread_world(m, 0);
    m.memory.write32(kR13 + 0x5AE4u, 0u);
    require(serve_reschedule(m.cpu) && !m.cpu.stopped(), "__OSReschedule with the hint clear is nothing"); ++checks;
    m.memory.write32(kR13 + 0x5AE4u, 1u);
    require(serve_reschedule(m.cpu) && m.cpu.stopped(), "__OSReschedule with the hint set must stop"); ++checks;
    return checks;
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: os_threads_contract_tests main.dol");
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");

        Machine original(dol), native(dol);
        struct Case { u32 entry; bool (*serve)(Cpu&, u32); const char* name; };
        const Case cases[] = {{kDisableScheduler, serve_disable_scheduler, "OSDisableScheduler"},
                              {kEnableScheduler, serve_enable_scheduler, "OSEnableScheduler"}};

        const u32 counts[] = {0u, 1u, 2u, 7u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu};
        unsigned runs = 0, mismatches[3] = {0, 0, 0};
        for (std::size_t ci = 0; ci < 2; ++ci) {
            const Case& c = cases[ci];
            const auto real = c.serve;
            const Serve variants[4] = {
                Serve(real),
                // wrong 1: the count is restored to its old value
                Serve([real](Cpu& cpu, u32 e) {
                    const u32 w = cpu.mem_read32(cpu.gpr[13] + kSchedulerCountSda);
                    real(cpu, e);
                    cpu.mem_write32(cpu.gpr[13] + kSchedulerCountSda, w);
                    return true;
                }),
                // wrong 2: the opposite direction
                Serve([ci](Cpu& cpu, u32 e) {
                    return ci == 0 ? serve_enable_scheduler(cpu, e) : serve_disable_scheduler(cpu, e);
                }),
                // wrong 3: MSR[EE] is left cleared
                Serve([real](Cpu& cpu, u32 e) {
                    real(cpu, e);
                    cpu.set_msr(cpu.msr() & ~kEnableBit);
                    return true;
                })};
            for (u32 count : counts)
                for (bool interrupts : {false, true})
                    for (u32 salt = 0; salt < 3; ++salt) {
                        original.prepare(count, interrupts, salt);
                        original.run_original(c.entry);
                        const Result expected = observe(original);
                        require(expected.regs[0] == count, "the original does not return the previous count");
                        for (int v = 0; v < 4; ++v) {
                            native.prepare(count, interrupts, salt);
                            require(!native.cpu.interrupt_pending(), "unexpected pending interrupt");
                            require(variants[v](native.cpu, c.entry), "owner refused with nothing pending");
                            const bool same = observe(native) == expected;
                            if (v == 0) {
                                if (!same) std::fprintf(stderr, "differs: %s count %08X ee %d salt %u\n", c.name, count, interrupts ? 1 : 0, salt);
                                require(same, c.name);
                            } else if (!same) {
                                ++mismatches[v - 1];
                            }
                        }
                        ++runs;
                    }
        }
        for (int v = 0; v < 3; ++v) require(mismatches[v] > 0, "a deliberately wrong owner was not detected");
        unsigned reader_runs = 0, reader_mismatches[3] = {0, 0, 0};
        run_readers(original, native, reader_runs, reader_mismatches);
        for (int v = 0; v < 3; ++v) require(reader_mismatches[v] > 0, "a deliberately wrong reader was not detected");
        std::printf("os_threads restore context: %u checks (registers, SRR, EXC flag, clamp, negative control)\n", run_restore_context(dol));
        std::printf("os_threads lifecycle: %u checks (stops naming the call, __OSReschedule)\n", run_lifecycle(dol));
        std::printf("os_threads yield identity: %u checks (identity, 8 negative controls, 2 no-switch cases)\n", run_yield_identity(dol));
        std::printf("os_threads waits: %u checks (wake, sleep with native events, yield, loud stops)\n", run_waits(dol));
        std::printf("os_threads chaining: %u checks (earlier hook serves / declines, four rows)\n", run_chain(dol));
        std::printf("os_threads readers: %u inputs equal; negative controls detected %u / %u / %u times\n", reader_runs,
                    reader_mismatches[0], reader_mismatches[1], reader_mismatches[2]);
        std::printf("os_threads contract: %u inputs equal; negative controls detected %u / %u / %u times\n", runs,
                    mismatches[0], mismatches[1], mismatches[2]);
        std::puts("os_threads contract tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
