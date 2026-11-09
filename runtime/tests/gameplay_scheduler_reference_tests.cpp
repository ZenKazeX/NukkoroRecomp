// The gameplay scheduler against the original instructions.
//
// Two bindings share one traversal (runtime/include/gameplay_traversal.h) and
// both are checked here against the only authority there is: the original
// scheduler at 0x8004ECAC, executing its own instruction words in the reference
// interpreter over a task tree built in guest memory.
//
//   A. shadow::gameplay::TaskSystem  - the address-free native task system.
//      Its callback and lifetime traces must equal the original's.
//   B. gekko::GameplayScheduler      - the scheduler the game runs on. The HLE
//      replaces 0x8004ECAC outright, so nothing of the original traversal
//      executes; task bodies are still reached by the same call. Its traces,
//      the final bytes of every task object, the scheduler's own globals
//      and, when the FPU is lost mid-walk, the exception sequence (vector,
//      SRR0, SRR1, place among the task calls) must all equal the
//      original's. The walk accounts no instructions: guest code runs in
//      zero game time (docs/native-time-20261005/DESIGN.md), so the game
//      clock, which no walk - native or original - may move, is what
//      replaced the retired instruction count.
//
// The fixture hooks are test scaffolding: they never enter the scheduler or the
// shipping executable.

#include "../include/native_gameplay.h"

#include "gekko/cpu.h"
#include "gekko/gameplay_scheduler.h"
#include "gekko/guest_memory.h"
#include "gekko/recomp.h"
#include "support/os_entry.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using gekko::u8;
using gekko::u16;
using gekko::u32;
using gekko::u64;
using namespace shadow::gameplay;
constexpr u32 base = 0x81600000u, table = 0x816F0000u, entry = 0x8004ECACu, finish = 0x817FF000u;
constexpr u32 kDepthCounter = 0x805EF270u, kLiveCount = 0x805EF278u;
// The FP-unavailable handler the fixture registers (no code: a host hook), and
// the OSContext current before a case makes its own.
constexpr u32 kFpHandler = 0x816F0000u;
constexpr u32 kEntryContext = 0x816D0000u;
// L_8004ECDC, the loop body: the original reaches it for its first visit
// straight from its first loop back edge (the bne at 0x8004EDC8).
constexpr u32 kFirstVisit = 0x8004ECDCu;
// The native walk's continuation (0x8004ECAC's second instruction, which the
// replaced original never reaches): its back-edge yields and its own lazy-FPU
// exceptions resume the walk there.
constexpr u32 kContinuation = 0x8004ECB0u;
// The walk's floating-point instructions (verified in run_fpu_case). Where the
// original takes a lazy-FPU exception at one of them, the native walk takes it
// at the same point of the traversal with SRR0 = kContinuation.
constexpr std::array<u32, 5> kWalkFpInstructions{{0x8004ED24u, 0x8004ED80u, 0x8004ED94u,
                                                  0x8004EDA8u, 0x8004EDD8u}};

void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

// The game clock: now, every deadline and the time-base view. Guest code runs
// in zero game time, so no walk, native or original, may move it.
struct ClockState {
    u64 now = 0;
    std::array<u64, static_cast<std::size_t>(gekko::GameEvent::Count)> deadlines{};
    u64 time_base = 0;
    bool operator==(const ClockState& other) const {
        return now == other.now && deadlines == other.deadlines && time_base == other.time_base;
    }
};
ClockState clock_of(const gekko::Cpu& cpu) {
    return {cpu.game_clock().now(), cpu.game_clock().deadlines(), cpu.time_base()};
}

// One machine with the original image loaded, the fixture hooks installed, and
// optionally the native scheduler in front of the original traversal.
struct Machine {
    gekko::GuestMemory bus;
    gekko::Cpu cpu{bus};
    std::unique_ptr<gekko::GameplayScheduler> scheduler;
    std::vector<int> trace;
    std::set<unsigned> deleted;
    bool self_kill = false;

    Machine(const char* path, bool native) {
        std::ifstream input(path, std::ios::binary);
        std::vector<u8> data{std::istreambuf_iterator<char>(input), {}};
        require(data.size() >= 256u, "missing original DOL");
        const auto word = [&](std::size_t offset) { return gekko::load_be32(data.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(std::uint64_t(offset) + size <= data.size(), "invalid DOL extent");
            require(address >= gekko::GuestMemory::kImageBase &&
                        std::uint64_t(address) + size <= gekko::GuestMemory::kImageEnd,
                    "invalid original section mapping");
            bus.write_block(address, data.data() + offset, size);
        }
        require(bus.read32(entry) == 0x9421FFE0u &&
                bus.read32(0x8004ED0Cu) == 0x408200A8u &&
                bus.read32(0x8004EF18u) == 0x9421FFF0u, "unexpected scheduler instruction evidence");
        const u32 expected[] = {0,1,2,3,4,5,6,7,8,9,10,11,12,14,13,15,16,17,18};
        for (u32 i = 0; i < 19u; ++i)
            require(bus.read32(0x8053ED6Cu + 4u * i) == expected[i], "unexpected original layer table");
        // The OS's FP-unavailable entry as __OSExceptionInit leaves it (built
        // from the DOL's own vector template) and a current OSContext for the
        // native entry to save the interrupted state into.
        test_support::install_os_exception(bus, gekko::Exception::FpUnavailable, 7u, kFpHandler);
        bus.write32(test_support::kOsCurrentContext, kEntryContext);
        cpu.install_hle(0x8004EA80u, [&](gekko::Cpu& c) {
            const unsigned id = (c.gpr[3] - base) / 64u;
            trace.push_back(static_cast<int>(id));
            if (self_kill && id % 5u == 0u) c.mem_write16(c.gpr[3] + 4u, c.mem_read16(c.gpr[3] + 4u) | 1u);
            return true;
        }, "reference task action");
        cpu.install_hle(0x8004EF18u, [&](gekko::Cpu& c) {
            const unsigned id = (c.gpr[3] - base) / 64u;
            trace.push_back(-static_cast<int>(id));
            deleted.insert(id);
            return false;
        }, "reference destructor observation");
        for (u32 address : {0x80050C40u, 0x80050BF8u, 0x803A1334u})
            cpu.install_hle(address, [](gekko::Cpu&) { return true; }, "reference fixture lock/free");
        // A native interrupt line whose handler declines every delivery (pc
        // stays, as when a source went away). initialize() holds it up with
        // MSR[EE] set, so needs_dispatch() is true at every back edge and the
        // native walk yields at every one, where the original polls. Nothing
        // is ever delivered, in either engine.
        cpu.set_native_interrupt_handler([](gekko::Cpu&) {});
        if (native) {
            // The scheduler must be installed last: its hook at 0x8004ECAC is
            // what the traversal call reaches instead of the original body.
            scheduler = std::make_unique<gekko::GameplayScheduler>(cpu, bus);
            scheduler->install();
        }
    }

    void initialize(const std::vector<unsigned>& parents, const std::vector<u16>& flags, bool paused, bool kill) {
        cpu.reset();
        cpu.attach_recompiled(nullptr);
        cpu.gpr[1] = 0x817E0000u;
        cpu.gpr[2] = 0x805FA780u;
        cpu.gpr[13] = 0x805EC500u;
        cpu.set_msr(cpu.msr() | gekko::kMsrFP | gekko::kMsrEE);
        cpu.set_native_interrupt(true);  // held up; see the constructor
        self_kill = kill;
        trace.clear();
        deleted.clear();
        bus.clear_block(base, (parents.size() + 1u) * 64u);
        cpu.mem_write32(table + 8u, 0x8004EF18u);
        cpu.mem_write32(table + 12u, 0x8004EA80u);
        cpu.mem_write32(0x805EF270u, 0u);
        cpu.mem_write32(0x805EF274u, 0u);
        cpu.mem_write32(0x805EF278u, static_cast<u32>(parents.size() + 1u));
        cpu.mem_write8(0x805EF27Cu, paused ? 1u : 0u);
        for (std::size_t i = 0; i < parents.size(); ++i) {
            const u32 address = base + static_cast<u32>(i + 1u) * 64u;
            const u32 parent = base + parents[i] * 64u;
            cpu.mem_write32(address + 16u, parent);
            cpu.mem_write32(address + 24u, table);
            cpu.mem_write16(address + 4u, flags[i]);
            const u32 first = cpu.mem_read32(parent + 20u);
            if (first == 0u) {
                cpu.mem_write32(parent + 20u, address);
                cpu.mem_write32(address + 8u, address);
            } else {
                const u32 tail = cpu.mem_read32(first + 8u);
                cpu.mem_write32(address + 8u, tail);
                cpu.mem_write32(tail + 12u, address);
                cpu.mem_write32(first + 8u, address);
            }
        }
    }
    // One call with the FPU disabled, stopped at the moment the exception is
    // taken. The original's fourth instruction is `stfd f31,24(r1)`, so the
    // frame is already pushed and the link already saved when the machine
    // faults; the scheduler has to leave exactly that state behind or the
    // handler returns into a frame that was never built.
    void fault_with_fpu_off() {
        cpu.pc = entry;
        cpu.lr = finish;
        cpu.gpr[3] = base;
        cpu.set_msr(cpu.msr() & (~gekko::kMsrFP));
        const u32 before = cpu.gpr[1];
        unsigned steps = 0;
        // Single instructions, stopping the moment control leaves the prologue:
        // the original walks its first three instructions and faults on the
        // fourth, the scheduler serves all four at once, and the comparison is
        // the machine state right after the exception is raised - not after the
        // game's handler, which this fixture has no context for.
        while (cpu.pc >= entry && cpu.pc <= entry + 0xCu && !cpu.stopped() && steps++ < 8u)
            cpu.step();
        require(!cpu.stopped(), "the machine stopped instead of taking the FP exception");
        require(cpu.gpr[1] == before - 32u, "the frame was not pushed before the FP exception");
        cpu.set_msr(cpu.msr() | (gekko::kMsrFP));
    }
    void pump() {
        cpu.pc = entry;
        cpu.lr = finish;
        cpu.gpr[3] = base;
        const double delta = 0.125;
        std::memcpy(&cpu.fpr[1].bits[0], &delta, sizeof(delta));
        const ClockState clock = clock_of(cpu);
        unsigned steps = 0;
        // One dispatch per call - one interpreted instruction or one hook -
        // through the dispatcher rather than step(): the scheduler hands
        // control back the way the original does, and the dispatcher is what
        // resumes it. The held native line keeps needs_dispatch() true, so
        // every back edge takes the yield path and the continuation is
        // exercised on every node rather than never.
        while (cpu.pc != finish && !cpu.stopped() && steps++ < 4000000u) cpu.run_steps(1u);
        if (cpu.stopped() || cpu.pc != finish) {
            std::fprintf(stderr, "stop=%d pc=0x%08X lr=0x%08X r1=0x%08X r3=0x%08X steps=%u detail=%s\n",
                         cpu.stopped() ? 1 : 0, cpu.pc, cpu.lr, cpu.gpr[1], cpu.gpr[3], steps,
                         cpu.stop().detail.c_str());
            if (scheduler) {
                const auto s = scheduler->stats();
                std::fprintf(stderr, "  traversals=%llu levels=%llu visits=%llu exec=%llu destroy=%llu "
                                     "mark=%llu yields=%llu faults=%llu\n",
                             (unsigned long long)s.traversals, (unsigned long long)s.levels,
                             (unsigned long long)s.nodes_visited, (unsigned long long)s.executes,
                             (unsigned long long)s.destroys, (unsigned long long)s.marks,
                             (unsigned long long)s.yields, (unsigned long long)s.faults);
            }
        }
        require(cpu.pc == finish && !cpu.stopped(), "scheduler failed to return");
        require(clock_of(cpu) == clock, "a scheduler pump moved the game clock");
    }
    // Every byte of the task arena, so a link or flag the two engines leave
    // differently is caught even when no callback order changed.
    std::vector<u8> arena(std::size_t nodes) {
        std::vector<u8> bytes((nodes + 1u) * 64u);
        for (std::size_t i = 0; i < bytes.size(); i += 4u) {
            const u32 value = cpu.mem_read32(base + static_cast<u32>(i));
            bytes[i] = static_cast<u8>(value >> 24);
            bytes[i + 1] = static_cast<u8>(value >> 16);
            bytes[i + 2] = static_cast<u8>(value >> 8);
            bytes[i + 3] = static_cast<u8>(value);
        }
        return bytes;
    }
};

enum class FpuLoss { BeforeVisit, AfterExecute, BeforeDestroy, AfterMark };

// One lazy-FPU exception as the fixture's 0x800 handler takes it: the vector,
// SRR0, SRR1 and how many task calls ran before it - its place in the
// traversal, which the retired-instruction count used to give.
struct FpuException {
    u32 vector = 0;  // where the OS entry continued: the handler
    u32 srr0 = 0;
    u32 srr1 = 0;
    std::size_t calls_before = 0;
    bool operator==(const FpuException& other) const {
        return vector == other.vector && srr0 == other.srr0 && srr1 == other.srr1 &&
               calls_before == other.calls_before;
    }
};

struct FpuResult {
    std::vector<u64> arguments;
    std::vector<FpuException> exceptions;
    std::vector<u32> exception_depths;
    std::vector<u8> objects;
    u64 restored_f31 = 0;
    ClockState clock;
};

// The native walk's exceptions for the original's sequence: the same ones,
// except that an exception the original takes inside its own walk resumes the
// native walk through its continuation.
std::vector<FpuException> as_served(std::vector<FpuException> exceptions) {
    for (FpuException& exception : exceptions) {
        if (std::find(kWalkFpInstructions.begin(), kWalkFpInstructions.end(), exception.srr0) !=
            kWalkFpInstructions.end())
            exception.srr0 = kContinuation;
    }
    return exceptions;
}

FpuResult run_fpu_case(const char* path, bool native, FpuLoss loss, bool paused, bool nested,
                       bool instrumented, bool retry) {
    Machine machine(path, native);
    auto& cpu = machine.cpu;
    const std::vector<unsigned> parents = nested ? std::vector<unsigned>{0, 1, 0}
                                                : std::vector<unsigned>{0, 0};
    std::vector<u16> flags(parents.size(), 0u);
    if (loss == FpuLoss::BeforeDestroy || loss == FpuLoss::AfterMark) flags[0] = 1u;
    if (loss == FpuLoss::BeforeDestroy) flags.back() = 2u;
    machine.initialize(parents, flags, paused, false);
    cpu.mem_write32(0x805EF274u, instrumented ? 1u : 0u);
    for (const auto& instruction : std::array<std::pair<u32, u32>, 6>{{
             {0x8004ECB8u, 0xDBE10018u}, {0x8004ED24u, 0xFC20F890u},
             {0x8004ED80u, 0xFC20F890u}, {0x8004ED94u, 0xFC20F890u},
             {0x8004EDA8u, 0xFC20F890u}, {0x8004EDD8u, 0xCBE10018u}}})
        require(cpu.mem_read32(instruction.first) == instruction.second,
                "unexpected original scheduler floating-point instruction");
    constexpr u64 delta = 0x3FC0000000000000ull;
    constexpr u64 caller_f31 = 0x4022000000000000ull;
    constexpr u64 foreign_f1 = 0x4060000000000000ull;
    constexpr u64 foreign_f31 = 0x4070000000000000ull;
    constexpr u32 context = 0x816E0000u;
    bool lost = false, pending = false, defer_reload = retry;
    FpuResult result;
    const auto lose_fpu = [&] {
        if (lost) return;
        lost = pending = true;
        cpu.mem_write32(0x800000D4u, context);
        cpu.mem_write32(0x800000D8u, 0u);
        cpu.mem_write16(context + 0x1A2u, 1u);
        cpu.mem_write64(context + 0x98u, 0xC066800000000000ull);
        cpu.mem_write64(context + 0x188u, cpu.fpr[31].bits[0]);
        cpu.fpr[1].bits[0] = foreign_f1;
        cpu.fpr[31].bits[0] = foreign_f31;
        cpu.set_msr(cpu.msr() & (~gekko::kMsrFP));
    };
    // The OS's FP-unavailable handler (OSSwitchFPUContext, __OSExceptionTable[7]) as this fixture
    // models it. The native OS entry enters it with r4 = the current OSContext, where it saved the
    // interrupted r3-r5, CR, LR, CTR, XER, SRR0 and SRR1; it reloads the FPU context (or, deferred,
    // does not) and returns as OSSwitchFPUContext does: everything from the context, the context's
    // exception flag cleared, SRR1 with MSR[FP] once the FPU is reloaded.
    cpu.install_hle(kFpHandler, [&](gekko::Cpu& c) {
        require(pending, "unexpected FP-unavailable exception in scheduler fixture");
        require(c.fpr[1].bits[0] == foreign_f1 && c.fpr[31].bits[0] == foreign_f31,
                "scheduler wrote floating-point registers before acquiring FPU ownership");
        const u32 saved = c.gpr[4];
        result.exceptions.push_back(
            {c.pc, c.mem_read32(saved + 0x198u), c.mem_read32(saved + 0x19Cu), result.arguments.size()});
        result.exception_depths.push_back(c.mem_read32(kDepthCounter));
        u32 srr1 = c.mem_read32(saved + 0x19Cu);
        if (defer_reload) {
            defer_reload = false;
        } else {
            c.fpr[1].bits[0] = c.mem_read64(context + 0x98u);
            c.fpr[31].bits[0] = c.mem_read64(context + 0x188u);
            c.mem_write32(0x800000D8u, context);
            pending = false;
            srr1 |= gekko::kMsrFP;
        }
        c.cr = c.mem_read32(saved + 0x80u);
        c.lr = c.mem_read32(saved + 0x84u);
        c.spr[gekko::SPR_SRR0] = c.mem_read32(saved + 0x198u);
        c.ctr = c.mem_read32(saved + 0x88u);
        c.xer = c.mem_read32(saved + 0x8Cu);
        c.mem_write16(saved + 0x1A2u, static_cast<u16>(c.mem_read16(saved + 0x1A2u) & ~2u));
        c.gpr[5] = c.mem_read32(saved + 0x14u);
        c.gpr[3] = c.mem_read32(saved + 0x0Cu);
        c.gpr[4] = c.mem_read32(saved + 0x10u);
        c.spr[gekko::SPR_SRR1] = srr1;
        gekko::recomp::return_from_interrupt(c, kFpHandler);
        return true;
    }, "fixture lazy FPU context reload");
    const u32 callback = instrumented ? 0x8004EDF0u : 0x8004EA80u;
    cpu.install_hle(callback, [&](gekko::Cpu& c) {
        if ((c.msr() & gekko::kMsrFP) == 0u) {
            gekko::recomp::fp_unavailable(c, callback);
            return true;
        }
        result.arguments.push_back(c.fpr[1].bits[0]);
        if (loss == FpuLoss::AfterExecute) lose_fpu();
        return true;
    }, "fixture task delta observation");
    cpu.install_hle(0x8004EF18u, [&](gekko::Cpu&) {
        if (loss == FpuLoss::BeforeDestroy) lose_fpu();
        return false;
    }, "fixture destructor FPU loss");
    cpu.install_hle(0x8004EECCu, [&](gekko::Cpu&) {
        if (loss == FpuLoss::AfterMark) lose_fpu();
        return false;
    }, "fixture subtree marking FPU loss");
    cpu.pc = entry;
    cpu.lr = finish;
    cpu.gpr[3] = base;
    cpu.fpr[1].bits[0] = delta;
    cpu.fpr[31].bits[0] = caller_f31;
    const ClockState clock = clock_of(cpu);
    // BeforeVisit loses the FPU at the first loop back edge, before the first
    // node is visited (once 17 instructions had retired, in both engines):
    // the original about to run L_8004ECDC for the first time, the native
    // walk just handed back at that back edge - its first yield.
    const auto at_first_visit = [&] {
        return machine.scheduler ? cpu.pc == kContinuation && machine.scheduler->stats().yields == 1u
                                 : cpu.pc == kFirstVisit;
    };
    unsigned steps = 0;
    while (cpu.pc != finish && !cpu.stopped() && steps++ < 100000u) {
        cpu.run_steps(1u);
        if (loss == FpuLoss::BeforeVisit && at_first_visit()) lose_fpu();
    }
    require(cpu.pc == finish && !cpu.stopped(), "lazy FPU scheduler case did not return");
    require(lost && !pending && result.exceptions.size() == (retry ? 2u : 1u),
            "lazy FPU case was not exercised");
    for (u64 argument : result.arguments) {
        if (argument != delta)
            std::fprintf(stderr, "lazy FPU: native=%d loss=%u paused=%d nested=%d argument=%016llX expected=%016llX\n",
                         native, static_cast<unsigned>(loss), paused, nested,
                         static_cast<unsigned long long>(argument), static_cast<unsigned long long>(delta));
        require(argument == delta, "task received stale frame delta");
    }
    require(cpu.fpr[31].bits[0] == caller_f31, "scheduler failed to restore caller f31");
    require(cpu.gpr[1] == 0x817E0000u && cpu.mem_read32(kDepthCounter) == 0u,
            "lazy FPU scheduler case left an unbalanced frame or recursion depth");
    if (machine.scheduler) require(!machine.scheduler->busy() && machine.scheduler->stats().faults == 0u,
                                   "lazy FPU scheduler traversal did not finish cleanly");
    result.objects = machine.arena(parents.size());
    result.restored_f31 = cpu.fpr[31].bits[0];
    result.clock = clock_of(cpu);
    require(result.clock == clock, "the lazy FPU scheduler case moved the game clock");
    return result;
}

void test_lazy_fpu(const char* path) {
    unsigned cases = 0;
    for (bool retry : {false, true}) for (bool instrumented : {false, true}) for (bool nested : {false, true}) {
        for (FpuLoss loss : {FpuLoss::BeforeVisit, FpuLoss::AfterExecute,
                            FpuLoss::BeforeDestroy, FpuLoss::AfterMark}) {
            if (loss == FpuLoss::AfterMark && !nested) continue;
            for (bool paused : {false, true}) {
                if (paused && loss != FpuLoss::BeforeVisit) continue;
                const auto original = run_fpu_case(path, false, loss, paused, nested, instrumented, retry);
                const auto served = run_fpu_case(path, true, loss, paused, nested, instrumented, retry);
                require(served.arguments == original.arguments, "lazy FPU task arguments differ from original");
                const std::vector<FpuException> expected = as_served(original.exceptions);
                if (served.exceptions != expected) {
                    const FpuException none{};
                    for (std::size_t i = 0; i < std::max(served.exceptions.size(), expected.size()); ++i) {
                        const FpuException& s = i < served.exceptions.size() ? served.exceptions[i] : none;
                        const FpuException& e = i < expected.size() ? expected[i] : none;
                        std::fprintf(stderr,
                                     "lazy FPU loss=%u paused=%d nested=%d instrumented=%d retry=%d "
                                     "exception %zu: served %08X srr0=%08X srr1=%08X calls=%zu | "
                                     "expected %08X srr0=%08X srr1=%08X calls=%zu\n",
                                     static_cast<unsigned>(loss), paused, nested, instrumented, retry, i,
                                     s.vector, s.srr0, s.srr1, s.calls_before, e.vector, e.srr0, e.srr1,
                                     e.calls_before);
                    }
                }
                require(served.exceptions == expected,
                        "lazy FPU exceptions (handler, SRR0, SRR1, place among the task calls) differ "
                        "from original");
                require(served.exception_depths == original.exception_depths,
                        "lazy FPU exception recursion depths differ from original");
                require(served.objects == original.objects && served.restored_f31 == original.restored_f31,
                        "lazy FPU final task or floating-point state differs from original");
                require(served.clock == original.clock, "lazy FPU game clocks differ from original");
                ++cases;
            }
        }
    }
    std::printf("PASS %u lazy-FPU scenarios against the original scheduler instructions\n", cases);
}

struct Recorded : Task {
    std::vector<int>& trace;
    int id;
    bool kill;
    Recorded(std::vector<int>& values, int tag, bool remove) : trace(values), id(tag), kill(remove) {}
    ~Recorded() override { trace.push_back(-id); }
    void update(UpdateContext& context) override {
        trace.push_back(id);
        if (kill && id % 5 == 0) context.tasks.request_destroy(context.self);
    }
};
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: gameplay_scheduler_reference_tests <original-main.dol>");
        test_lazy_fpu(argv[1]);
        auto original = std::make_unique<Machine>(argv[1], false);
        auto served = std::make_unique<Machine>(argv[1], true);
        shadow::execution::Executor jobs(2);
        constexpr u16 flag_choices[] = {0,2,4,8,0x100,0x102,1,3,5,0x101};
        unsigned cases = 0;
        u64 yields = 0;
        for (unsigned seed = 0; seed < 80u; ++seed) for (unsigned pause = 0; pause < 2u; ++pause) {
            std::vector<int> trace;
            TaskSystem native(jobs);
            std::vector<TaskId> ids{native.layer(Layer::System)};
            std::vector<unsigned> parents;
            std::vector<u16> flags;
            const bool self_kill = seed % 3u == 0u;
            for (unsigned i = 1; i <= 24u; ++i) {
                const unsigned parent = (seed * 11u + i * 7u) % i;
                const u16 state = flag_choices[(seed + i) % 10u];
                parents.push_back(parent);
                flags.push_back(state);
                ids.push_back(native.create<Recorded>(ids[parent], "verified task",
                    {(state & 0x100u) != 0u, (state & 0xEu) == 0u}, trace, static_cast<int>(i), self_kill));
                if (state & 1u) native.request_destroy(ids.back());
            }
            native.set_paused(pause != 0u);
            original->initialize(parents, flags, pause != 0u, self_kill);
            served->initialize(parents, flags, pause != 0u, self_kill);
            const u64 yields_before = served->scheduler->stats().yields;
            for (unsigned step = 0; step < 3u; ++step) {
                native.update_children(ids[0], 0.125f);
                original->pump();
                served->pump();
                if (trace != original->trace) {
                    std::fprintf(stderr, "native task trace mismatch seed=%u paused=%u step=%u native=%zu original=%zu\n",
                                 seed, pause, step, trace.size(), original->trace.size());
                    throw std::runtime_error("native task behavior differs from original instructions");
                }
                if (served->trace != original->trace) {
                    std::fprintf(stderr, "scheduler trace mismatch seed=%u paused=%u step=%u served=%zu original=%zu\n",
                                 seed, pause, step, served->trace.size(), original->trace.size());
                    throw std::runtime_error("native scheduler callback order differs from original instructions");
                }
                require(served->deleted == original->deleted, "native scheduler task lifetimes differ");
                require(served->arena(parents.size()) == original->arena(parents.size()),
                        "native scheduler left different bytes in the task objects");
                require(served->cpu.mem_read32(kDepthCounter) == original->cpu.mem_read32(kDepthCounter) &&
                        served->cpu.mem_read32(kLiveCount) == original->cpu.mem_read32(kLiveCount),
                        "native scheduler left different scheduler globals");
                require(clock_of(served->cpu) == clock_of(original->cpu),
                        "native scheduler and original traversal left different game clocks");
                for (unsigned i = 1; i < ids.size(); ++i)
                    require(native.alive(ids[i]) == (original->deleted.count(i) == 0u), "native task lifetime differs");
            }
            const auto stats = served->scheduler->stats();
            require(stats.faults == 0u, "the native scheduler refused a traversal");
            require(stats.yields > yields_before, "the native scheduler never yielded at a back edge");
            yields = stats.yields;
            if (seed == 0u) {
                // Both engines take the floating-point-unavailable exception at
                // the same instruction, leaving the same machine state.
                original->initialize(parents, flags, pause != 0u, self_kill);
                served->initialize(parents, flags, pause != 0u, self_kill);
                original->fault_with_fpu_off();
                served->fault_with_fpu_off();
                if (served->cpu.pc != original->cpu.pc ||
                    served->cpu.spr[gekko::SPR_SRR0] != original->cpu.spr[gekko::SPR_SRR0]) {
                    std::fprintf(stderr,
                                 "fp fault: served pc=0x%08X srr0=0x%08X srr1=0x%08X r1=0x%08X | "
                                 "original pc=0x%08X srr0=0x%08X srr1=0x%08X r1=0x%08X\n",
                                 served->cpu.pc, served->cpu.spr[gekko::SPR_SRR0],
                                 served->cpu.spr[gekko::SPR_SRR1], served->cpu.gpr[1],
                                 original->cpu.pc, original->cpu.spr[gekko::SPR_SRR0],
                                 original->cpu.spr[gekko::SPR_SRR1], original->cpu.gpr[1]);
                }
                require(served->cpu.pc == original->cpu.pc &&
                        served->cpu.gpr[1] == original->cpu.gpr[1] &&
                        served->cpu.spr[gekko::SPR_SRR0] == original->cpu.spr[gekko::SPR_SRR0] &&
                        served->cpu.spr[gekko::SPR_SRR1] == original->cpu.spr[gekko::SPR_SRR1] &&
                        served->arena(parents.size()) == original->arena(parents.size()),
                        "the scheduler takes the FP-unavailable exception differently");
            }
            ++cases;
        }
        std::printf("PASS %u task-tree scenarios, 3 updates each: the native task system and the native\n"
                    "     scheduler both match the original instruction traces; the scheduler also matches\n"
                    "     task bytes, scheduler globals and the game clock exactly (%llu back-edge yields)\n",
                    cases, static_cast<unsigned long long>(yields));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
