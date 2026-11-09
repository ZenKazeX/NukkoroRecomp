// liveness-roots: reference-interpreter-only
// The GameCube OS thread service as the game and the CRI middleware observe it (docs/NATIVE_THREADS.md): scripted
// scenarios over the public thread API, run on the ORIGINAL words of the game's DOL in the reference interpreter.
// Each thread body appends records to a trace buffer; the observable contract is that trace (which thread ran, in
// which order, with which return values) plus the API's return values - never OSThread internals, which the native
// scheduler does not keep in guest memory.
//
// Stage 1 (this file today): the original only - it prints each scenario's trace and checks the facts the design
// relies on (priority preemption, FIFO within a priority, sleep / wakeup, suspend counts, exit, scheduler disable).
// Stage 2: the same scenarios on the native scheduler, traces compared record for record.
//
// usage: native_threads_tests <sys/main.dol>

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"

#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;

// ---- the thread API (PAL DOL; names from the SDK order, docs/NATIVE_THREADS.md)
constexpr u32 kThreadInit = 0x80378324u;       // __OSThreadInit
constexpr u32 kInitQueue = 0x8037847Cu;        // OSInitThreadQueue(queue)
constexpr u32 kCurrentThread = 0x8037848Cu;    // OSGetCurrentThread()
constexpr u32 kIsTerminated = 0x80378498u;     // OSIsThreadTerminated(thread)
constexpr u32 kDisableScheduler = 0x803784CCu; // OSDisableScheduler() -> previous count
constexpr u32 kEnableScheduler = 0x8037850Cu;  // OSEnableScheduler() -> previous count
constexpr u32 kYield = 0x80378A08u;            // OSYieldThread()
constexpr u32 kCreate = 0x80378A44u;           // OSCreateThread(thread, func, param, stack top, size, prio, attr)
constexpr u32 kResume = 0x80378ECCu;           // OSResumeThread(thread) -> previous suspend count
constexpr u32 kSuspend = 0x80379154u;          // OSSuspendThread(thread) -> previous suspend count
constexpr u32 kSleep = 0x803792C4u;            // OSSleepThread(queue)
constexpr u32 kWakeup = 0x803793B0u;           // OSWakeupThread(queue)
constexpr u32 kSetPriority = 0x803794B4u;      // OSSetThreadPriority(thread, prio) -> BOOL
constexpr u32 kGetPriority = 0x80379574u;      // OSGetThreadPriority(thread)

// ---- the test's own guest memory (MEM1, above the game's arena use in these scenarios)
constexpr u32 kFinish = 0x817FF000u, kMainStack = 0x817E0000u;
constexpr u32 kR13 = 0x805EC500u, kR2 = 0x805FA780u;
constexpr u32 kCode = 0x81700000u;      // thread bodies
constexpr u32 kTrace = 0x81710000u;     // u32 count, then u32 records
constexpr u32 kThreads = 0x81720000u;   // OSThread objects, 0x400 apart
constexpr u32 kQueues = 0x81730000u;    // OSThreadQueue (8 bytes each)
constexpr u32 kStacks = 0x81740000u;    // thread stacks, 0x4000 apart
constexpr u32 kStackSize = 0x4000u;

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
std::string hex(u32 v) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", v);
    return text;
}
int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// ---- a few PowerPC encodings for the thread bodies
u32 addi(u32 d, u32 a, s32 imm) { return (14u << 26) | (d << 21) | (a << 16) | (static_cast<u32>(imm) & 0xFFFFu); }
u32 addis(u32 d, u32 a, u32 imm) { return (15u << 26) | (d << 21) | (a << 16) | (imm & 0xFFFFu); }
u32 ori(u32 a, u32 s, u32 imm) { return (24u << 26) | (s << 21) | (a << 16) | (imm & 0xFFFFu); }
u32 lwz(u32 d, s32 off, u32 a) { return (32u << 26) | (d << 21) | (a << 16) | (static_cast<u32>(off) & 0xFFFFu); }
u32 stw(u32 s, s32 off, u32 a) { return (36u << 26) | (s << 21) | (a << 16) | (static_cast<u32>(off) & 0xFFFFu); }
u32 stwu(u32 s, s32 off, u32 a) { return (37u << 26) | (s << 21) | (a << 16) | (static_cast<u32>(off) & 0xFFFFu); }
u32 mr(u32 a, u32 s) { return (31u << 26) | (s << 21) | (a << 16) | (s << 11) | (444u << 1); }
u32 add(u32 d, u32 a, u32 b) { return (31u << 26) | (d << 21) | (a << 16) | (b << 11) | (266u << 1); }
u32 slwi(u32 a, u32 s, u32 n) { return (21u << 26) | (s << 21) | (a << 16) | (n << 11) | (0u << 6) | ((31u - n) << 1); }
u32 mflr(u32 d) { return (31u << 26) | (d << 21) | (8u << 16) | (339u << 1); }
u32 mtlr(u32 s) { return (31u << 26) | (s << 21) | (8u << 16) | (467u << 1); }
constexpr u32 kBlr = 0x4E800020u;
u32 bl(u32 from, u32 to) { return (18u << 26) | ((to - from) & 0x03FFFFFCu) | 1u; }

struct Machine {
    GuestMemory bus;
    Cpu cpu{bus};
    u32 code_end = kCode;

    explicit Machine(const std::vector<u8>& dol) {
        const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        require(bus.read32(kThreadInit) == 0x7C0802A6u && bus.read32(kYield) == 0x7C0802A6u,
                "unexpected thread service instruction evidence");
        cpu.reset();
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
        bus.write32(kTrace, 0u);
    }

    // A thread body: record(param), OSYieldThread, record(param + 0x100), OSSleepThread(queue = kQueues),
    // record(param + 0x200), return (the thread exits through the LR OSCreateThread gave it).
    u32 body_yield_sleep() {
        const u32 at = code_end;
        std::vector<u32> w;
        const auto emit = [&](u32 x) { w.push_back(x); };
        const auto here = [&] { return at + static_cast<u32>(w.size()) * 4u; };
        const auto record_r31_plus = [&](s32 delta) {  // trace[count++] = r31 + delta
            emit(addis(4, 0, kTrace >> 16));
            emit(ori(4, 4, kTrace & 0xFFFFu));
            emit(lwz(5, 0, 4));
            emit(addi(6, 5, 1));
            emit(stw(6, 0, 4));
            emit(slwi(5, 5, 2));
            emit(add(5, 5, 4));
            emit(addi(6, 31, delta));
            emit(stw(6, 4, 5));
        };
        emit(mflr(0));
        emit(stw(0, 4, 1));
        emit(stwu(1, -0x20, 1));
        emit(stw(31, 0x1C, 1));
        emit(mr(31, 3));
        record_r31_plus(0);
        emit(bl(here(), kYield));
        record_r31_plus(0x100);
        emit(addis(3, 0, kQueues >> 16));
        emit(ori(3, 3, kQueues & 0xFFFFu));
        emit(bl(here(), kSleep));
        record_r31_plus(0x200);
        emit(lwz(31, 0x1C, 1));
        emit(addi(1, 1, 0x20));
        emit(lwz(0, 4, 1));
        emit(mtlr(0));
        emit(kBlr);
        for (std::size_t i = 0; i < w.size(); ++i) bus.write32(at + static_cast<u32>(i) * 4u, w[i]);
        code_end = at + static_cast<u32>(w.size()) * 4u;
        return at;
    }

    // Call an API function as the current thread; true when it returned to us.
    bool call(u32 entry, std::initializer_list<u32> args, u32* result = nullptr) {
        u32 r = 3u;
        for (const u32 v : args) cpu.gpr[r++] = v;
        cpu.gpr[1] = kMainStack;
        cpu.lr = kFinish;
        cpu.pc = entry;
        u64 steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 20'000'000ull) cpu.run_steps(1u);
        if (result) *result = cpu.gpr[3];
        return cpu.pc == kFinish && !cpu.stopped();
    }

    std::vector<u32> trace() {
        std::vector<u32> out(bus.read32(kTrace));
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = bus.read32(kTrace + 4u + static_cast<u32>(i) * 4u);
        return out;
    }
};

std::string show(const std::vector<u32>& t) {
    std::string s;
    for (const u32 v : t) s += (s.empty() ? "" : " ") + hex(v).substr(6);
    return s;
}

std::vector<u8> read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}

// Scenario 1: preemption, FIFO yield, sleep / wakeup, exit. The main thread runs at priority 16 (__OSThreadInit).
//  T1 (prio 10) resumed: preempts main at once -> "01", yields (nobody of its priority) -> "101", sleeps.
//  T2 (prio 16) resumed: no preemption; main yields -> T2 "02", T2 yields -> main.
//  main wakes the queue -> T1 (prio 10) and T2 (when it sleeps) ... the exact order is the oracle's.
std::vector<u32> scenario_1(Machine& m, std::vector<std::string>& notes) {
    u32 r = 0;
    require(m.call(kThreadInit, {}), "__OSThreadInit did not return");
    require(m.call(kInitQueue, {kQueues}), "OSInitThreadQueue did not return");
    const u32 body = m.body_yield_sleep();
    const u32 t1 = kThreads, t2 = kThreads + 0x400u;
    require(m.call(kCreate, {t1, body, 1u, kStacks + kStackSize, kStackSize, 10u, 0u}, &r), "create T1");
    notes.push_back("create T1 -> " + std::to_string(r));
    require(m.call(kCreate, {t2, body, 2u, kStacks + 2u * kStackSize, kStackSize, 16u, 0u}, &r), "create T2");
    notes.push_back("create T2 -> " + std::to_string(r));
    require(m.call(kResume, {t1}, &r), "resume T1");
    notes.push_back("resume T1 -> " + std::to_string(r) + " trace " + show(m.trace()));
    require(m.call(kResume, {t2}, &r), "resume T2");
    notes.push_back("resume T2 -> " + std::to_string(r) + " trace " + show(m.trace()));
    require(m.call(kYield, {}), "main yield");
    notes.push_back("main yield -> trace " + show(m.trace()));
    require(m.call(kGetPriority, {t2}, &r), "get prio");
    notes.push_back("prio T2 " + std::to_string(r));
    require(m.call(kWakeup, {kQueues}), "wakeup");
    notes.push_back("wakeup -> trace " + show(m.trace()));
    require(m.call(kYield, {}), "main yield 2");
    notes.push_back("main yield 2 -> trace " + show(m.trace()));
    require(m.call(kWakeup, {kQueues}), "wakeup 2");
    notes.push_back("wakeup 2 -> trace " + show(m.trace()));
    require(m.call(kYield, {}), "main yield 3");
    require(m.call(kIsTerminated, {t1}, &r), "terminated T1");
    notes.push_back("T1 terminated " + std::to_string(r));
    require(m.call(kIsTerminated, {t2}, &r), "terminated T2");
    notes.push_back("T2 terminated " + std::to_string(r));
    return m.trace();
}

// Scenario 2: suspend counts, a priority raise that preempts, scheduler disable / enable, the current thread.
std::vector<u32> scenario_2(Machine& m, std::vector<std::string>& notes) {
    u32 r = 0;
    require(m.call(kThreadInit, {}), "__OSThreadInit did not return");
    require(m.call(kInitQueue, {kQueues}), "OSInitThreadQueue did not return");
    const u32 body = m.body_yield_sleep();
    const u32 t3 = kThreads + 0x800u;
    const auto step = [&](const std::string& what, u32 entry, std::initializer_list<u32> args) {
        require(m.call(entry, args, &r), what + " did not return");
        notes.push_back(what + " -> " + hex(r) + " trace " + show(m.trace()));
    };
    step("current thread", kCurrentThread, {});
    step("create T3 prio 20", kCreate, {t3, body, 3u, kStacks + 3u * kStackSize, kStackSize, 20u, 0u});
    step("resume T3", kResume, {t3});
    step("suspend T3", kSuspend, {t3});
    step("suspend T3 again", kSuspend, {t3});
    step("resume T3", kResume, {t3});
    step("resume T3", kResume, {t3});
    step("main yield (T3 is lower)", kYield, {});
    step("priority T3 -> 5", kSetPriority, {t3, 5u});
    step("get priority T3", kGetPriority, {t3});
    step("disable scheduler", kDisableScheduler, {});
    step("wakeup (scheduler off)", kWakeup, {kQueues});
    step("enable scheduler", kEnableScheduler, {});
    step("main yield", kYield, {});
    step("T3 terminated", kIsTerminated, {t3});
    return m.trace();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::printf("usage: native_threads_tests <sys/main.dol>\n");
        return 2;
    }
    try {
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() > 0x100u, "cannot read the DOL");
        Machine original(dol);
        std::vector<std::string> notes;
        const std::vector<u32> t = scenario_1(original, notes);
        for (const auto& n : notes) std::printf("  %s\n", n.c_str());
        std::printf("scenario 1 (original): %s\n", show(t).c_str());
        // The facts the native design relies on (priority 10 preempts 16 at resume; FIFO within 16).
        check(!t.empty() && t[0] == 0x01u, "T1 (priority 10) runs first, preempting main at its resume");
        Machine original2(dol);
        std::vector<std::string> notes2;
        const std::vector<u32> t2 = scenario_2(original2, notes2);
        for (const auto& n : notes2) std::printf("  %s\n", n.c_str());
        std::printf("scenario 2 (original): %s\n", show(t2).c_str());
    } catch (const std::exception& error) {
        std::printf("ERROR: %s\n", error.what());
        return 1;
    }
    std::printf("native threads: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
