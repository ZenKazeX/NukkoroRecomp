#include "gekko/cpu.h"
#include "gekko/cpu_diag.h"
#include "gekko/os_exception_vector.h"
#include "gekko/recomp.h"
#include "fallback_fixture.h"

#include <intrin.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace gekko;

// This executable links the PRODUCTION runtime (shadow_runtime): Cpu::run has no bound and runs until the
// machine stops, nothing is ever interpreted and nothing counts instructions. A run is therefore checked by
// what it leaves behind - the architected state, the stop it ended with, the native call trees the
// dispatcher entered (Cpu::recompiled_calls) - and by the native-time invariant: guest code runs in zero
// game time, so the time base, the decrementer and the game clock's schedule come out of every run exactly
// as they went in (docs/native-time-20261005/DESIGN.md).

namespace {
void require(bool ok, const char* detail) {
    if (!ok) throw std::runtime_error(detail);
}

constexpr u32 kEntry = 0x80008000u, kThunk = 0x80008200u, kMissing = 0x80009000u;
// The OS's exception entry as the guest installs it (__OSExceptionInit, OSSetCurrentContext): the
// current OSContext and one handler per exception this file takes. No handler has translated code, so
// production stops at the handler the native entry continues at.
constexpr u32 kContext = 0x8000A000u;
constexpr u32 kContextBytes = 0x2C8u;  // sizeof(OSContext)
constexpr u32 kExceptionTable = 0x80003000u;  // __OSExceptionTable
constexpr u32 kExternalHandler = 0x80009500u, kFpHandler = 0x80009800u, kDecrementerHandler = 0x80009900u;
// A reference fixture's device page (GuestMemory::register_mmio): production registers none.
constexpr u32 kDevicePage = 0xCC000000u;
// A clock that has already run, with a frame event pending: "no time passes" is checked against a schedule.
constexpr u64 kFixtureNow = 3u * kGameTicksPerSecond;
constexpr u64 kFixtureFrame = kFixtureNow + kGameTicksPerSecond / 200u;

struct ReadProbe final : MmioDevice {
    u64 reads = 0;
    const char* name() const override { return "native execution probe"; }
    u32 read32(u32) override { ++reads; return 0x38630001u; }
};

struct Fixture {
    ReadProbe mmio;
    GuestMemory bus;
    Cpu cpu{bus};
    std::vector<std::pair<u32, bool>> accesses;  // every guest access a run makes: (address, writing)
    explicit Fixture(RecompiledCode* code, u32 entry = kMissing) {
        cpu.reset(); cpu.attach_recompiled(code); cpu.set_entry(entry);
        for (u32 i = 0; i < 32u; ++i) {
            cpu.gpr[i] = 0x100u + i;
            cpu.fpr[i].bits[0] = 0x3FF0000000000000ull + i;
            cpu.fpr[i].bits[1] = 0x4000000000000000ull + i;
        }
        cpu.cr = 0x12345678u; cpu.xer = kXerSO | kXerCA;
        cpu.ctr = 0xABCDEF98u; cpu.fpscr = 0x1234u;
        cpu.gpr[5] = 0x80009200u;
        cpu.game_clock().set_now(kFixtureNow);
        cpu.game_clock().schedule(GameEvent::Frame, kFixtureFrame);
        cpu.lr = 0x80009100u; cpu.set_time_base(0x123456780ull); cpu.set_decrementer(100u);
        program(kEntry, 0x38630001u);
        bus.write32(kThunk, 0x38630001u); bus.write32(kMissing, 0x90650000u);
        bus.write32(0x80009200u, 0xCAFEBABEu);
        // The OS's entry: the vector page with `li r3, <index>` at +0x68, the handler in __OSExceptionTable, the context at low memory 0xD4. The
        // handlers hold original instruction bytes production must never fetch.
        for (const auto& [vector, index, handler] :
             {std::tuple{Exception::ExternalInterrupt, 4u, kExternalHandler},
              std::tuple{Exception::FpUnavailable, 7u, kFpHandler},
              std::tuple{Exception::Decrementer, 8u, kDecrementerHandler}}) {
            write_os_exception_vector(bus, 0x80000000u | static_cast<u32>(vector), index);
            bus.write32(kExceptionTable + 4u * index, handler);
            bus.write32(handler, 0x38630001u);
        }
        bus.write32(0x800000D4u, kContext);
        bus.register_mmio(kDevicePage, 0x100u, &mmio);
        bus.set_access_trace([this](u32 address, std::size_t, bool writing) {
            accesses.emplace_back(address, writing);
        });
    }
    void program(u32 address, u32 first) {
        bus.write32(address, first);
        bus.write32(address + 4u, 0x38840001u);
        bus.write32(address + 8u, 0x4E800020u);
    }
    // Production fetches no instruction bytes and touches no memory of its own: the only guest
    // accesses a run may make are the native OS exception entry's - the OSContext it saves the
    // interrupted state into, and its reads of the context pointer (0xD4) and of __OSExceptionTable.
    void no_fetch() const {
        for (const auto& [address, writing] : accesses) {
            const bool context = address >= kContext && address - kContext < kContextBytes;
            const bool os_word = !writing && (address == 0x800000D4u ||
                                              (address >= kExceptionTable && address < kExceptionTable + 0x30u));
            require(context || os_word, "a run accessed guest memory other than the OS exception entry's");
        }
        require(mmio.reads == 0u, "original instruction fetch read MMIO");
        require(bus.diagnostics().unmapped_reads == 0u, "original instruction fetch read unmapped memory");
    }
};

// The interrupted state the native OS entry saved in the OSContext.
u32 saved(const Fixture& f, u32 offset) { return f.bus.peek32(kContext + offset); }
u32 saved_srr0(const Fixture& f) { return saved(f, 0x198u); }
u32 saved_srr1(const Fixture& f) { return saved(f, 0x19Cu); }
u32 saved_gpr(const Fixture& f, u32 index) { return saved(f, 0x0Cu + 4u * (index - 3u)); }  // r3-r5
u32 saved_cr(const Fixture& f) { return saved(f, 0x80u); }

// Every mapped region's committed bytes, by base.
struct MemorySnapshot {
    std::vector<std::pair<u32, std::vector<u8>>> regions;
    explicit MemorySnapshot(const GuestMemory& memory) {
        for (const auto& region : memory.regions())
            regions.emplace_back(region.base, memory.window_bytes(region.base, region.base + region.committed));
    }
    bool operator==(const MemorySnapshot& other) const { return regions == other.regions; }
    u8* at(u32 address, u32 size) {
        for (auto& [base, bytes] : regions)
            if (address >= base && address - base + size <= bytes.size()) return bytes.data() + (address - base);
        throw std::runtime_error("the expected state names unmapped memory");
    }
    void write32(u32 address, u32 value) {
        u8* p = at(address, 4u);
        for (u32 i = 0; i < 4u; ++i) p[i] = static_cast<u8>(value >> (24u - 8u * i));
    }
    u16 read16(u32 address) { const u8* p = at(address, 2u); return static_cast<u16>((p[0] << 8) | p[1]); }
    void write16(u32 address, u16 value) {
        u8* p = at(address, 2u);
        p[0] = static_cast<u8>(value >> 8); p[1] = static_cast<u8>(value);
    }
};

struct State {
    u32 gpr[32];
    FpReg fpr[32];
    std::array<u32, 1024> spr;
    std::array<u32, 16> sr;
    u32 lr, msr, cr, xer, ctr, fpscr;
    // Game time: the time-base and decrementer views (Cpu::time_base, Cpu::decrementer - DEC is not an
    // spr[] slot) and the clock and schedule behind them. Only a wait point moves the clock and no run here
    // reaches one, so none of it may change.
    u64 time, now;
    u32 dec;
    std::array<u64, static_cast<std::size_t>(GameEvent::Count)> deadlines;
    // Native call trees the dispatcher entered. Native code has no instruction count, so a test states how
    // many native dispatches its run makes; the registers say what they did.
    u64 dispatches;
    MemorySnapshot memory;
    explicit State(Fixture& f)
        : spr(f.cpu.spr), sr(f.cpu.sr), lr(f.cpu.lr), msr(f.cpu.msr()), cr(f.cpu.cr),
          xer(f.cpu.xer), ctr(f.cpu.ctr), fpscr(f.cpu.fpscr), time(f.cpu.time_base()),
          now(f.cpu.game_clock().now()), dec(f.cpu.decrementer()),
          deadlines(f.cpu.game_clock().deadlines()), dispatches(f.cpu.recompiled_calls),
          memory(f.bus) {
        std::memcpy(gpr, f.cpu.gpr, sizeof(gpr));
        std::memcpy(fpr, f.cpu.fpr, sizeof(fpr));
    }
    void dispatched(u64 count) { dispatches += count; }
    // An exception taken with SRR0 = `resume`, as production takes it: the architected entry
    // (SRR1 = the MSR's saved bits, the exception MSR - ME alone from this fixture's MSR), then the
    // native OS entry (os_exception_entry.cpp; extract/exception-vectors.csv), whose rfi enters the
    // handler with translation on.
    void take_exception(u32 index, u32 handler, u32 resume) {
        const u32 interrupted_msr = msr & 0x87C0FFFFu;
        const u32 entry_msr = kMsrME | kMsrIR | kMsrDR;
        spr[SPR_SPRG0] = gpr[4];
        memory.write32(kContext + 0x0Cu, gpr[3]);
        memory.write32(kContext + 0x10u, gpr[4]);
        memory.write32(kContext + 0x14u, gpr[5]);
        memory.write16(kContext + 0x1A2u, static_cast<u16>(memory.read16(kContext + 0x1A2u) | 2u));
        memory.write32(kContext + 0x80u, cr);
        memory.write32(kContext + 0x84u, lr);
        memory.write32(kContext + 0x88u, ctr);
        memory.write32(kContext + 0x8Cu, xer);
        memory.write32(kContext + 0x198u, resume);
        memory.write32(kContext + 0x19Cu, interrupted_msr);
        const u32 cr0 = ((interrupted_msr & kMsrRI) != 0u ? 4u : 2u) | ((xer & kXerSO) != 0u ? 1u : 0u);
        cr = (cr & 0x0FFFFFFFu) | (cr0 << 28);
        gpr[3] = index;
        gpr[4] = kContext;
        gpr[5] = handler;
        spr[SPR_SRR0] = handler;
        spr[SPR_SRR1] = entry_msr;
        msr = entry_msr;
    }
    void check(Fixture& f) const {
        require(std::memcmp(gpr, f.cpu.gpr, sizeof(gpr)) == 0, "GPR mismatch");
        require(std::memcmp(fpr, f.cpu.fpr, sizeof(fpr)) == 0, "FPR mismatch");
        require(f.cpu.spr == spr && f.cpu.sr == sr && f.cpu.lr == lr && f.cpu.msr() == msr,
                "SPR/segment/LR/MSR mismatch");
        require(f.cpu.cr == cr && f.cpu.xer == xer && f.cpu.ctr == ctr && f.cpu.fpscr == fpscr,
                "CR/XER/CTR/FPSCR mismatch");
        require(f.cpu.recompiled_calls == dispatches && f.cpu.interpreted_instructions() == 0u,
                "native dispatch mismatch, or the native runtime interpreted an instruction");
        require(f.cpu.time_base() == time && f.cpu.decrementer() == dec &&
                    f.cpu.game_clock().now() == now && f.cpu.game_clock().deadlines() == deadlines,
                "guest execution moved game time (time base, decrementer or the clock's schedule)");
        require(MemorySnapshot(f.bus) == memory, "guest memory changed");
    }
};

void native_fault(const Fixture& f, u32 pc) {
    const auto& c = f.cpu;
    require(c.stopped() && c.stop().reason == StopReason::ExecutionFault, "expected native ExecutionFault");
    require(c.pc == pc && c.stop().address == pc, "native fault PC mismatch");
    char upper[16], lower[16];
    std::snprintf(upper, sizeof(upper), "%X", pc); std::snprintf(lower, sizeof(lower), "%x", pc);
    require(c.stop().detail.find("native") != std::string::npos &&
                (c.stop().detail.find(upper) != std::string::npos ||
                 c.stop().detail.find(lower) != std::string::npos), "native diagnostic must name the PC");
}

// A stop posted by another host thread (Cpu::request_stop_from_host, the wait-point watchdog's entry). The
// dispatcher serves it at its next boundary, ahead of the R1 recognition point and of any pending call: a
// run whose first boundary serves it makes no dispatch - what a zero instruction budget used to be.
void post_host_stop(Cpu& cpu, const char* detail) {
    std::thread host([&cpu, detail] { cpu.request_stop_from_host(StopReason::HostRequest, detail); });
    host.join();
}

struct ThunkTable {
    RecompiledCode::Entry entry;
    RecompiledCode code;
    explicit ThunkTable(RecompiledCode::Fn fn) : entry{kThunk, 4u, fn, "test-native"} {
        code.install(&entry, 1u);
    }
};
// Gave up at the very instruction it was dispatched to: nothing ran (Cpu::run's stall test is the leave
// site, as the instruction clock's "retired nothing" was).
void no_progress(Cpu& c) { c.leave_to_dispatcher(c.pc, FallbackReason::NotRecompiled, c.pc); }
void call_missing(Cpu& c) { recomp::call_indirect(c, kMissing, kThunk + 4u); }
void jump_missing(Cpu& c) { recomp::jump_indirect(c, kMissing); }
// A hand-back that resumes at the dispatch entry is progress whenever something ran first: it gave up at
// a later instruction (here its back edge, the next word), whatever it changed. Each entry here switches
// stacks, as an OS switch to a second thread created with the same entry routine does, and the twelfth
// asks the host to stop.
u64 g_yields = 0;
void yielding(Cpu& c) {
    c.gpr[1] += 0x100u;
    if (++g_yields >= 12u) c.request_stop(StopReason::HostRequest, "native yield");
    c.leave_to_dispatcher(c.pc, FallbackReason::Yield, c.pc + 4u);
}
// Gave up at its own entry instruction: nothing ran, dispatching it again would spin forever and
// production cannot interpret its instruction instead, so the machine stops there, whatever reason the
// body gave.
void stalled_yield(Cpu& c) { c.leave_to_dispatcher(c.pc, FallbackReason::Yield, c.pc); }
// A loop that resumes at its own head through the dispatcher every lap and changes only a counter -
// difftest F1b bnd shape 13: `addi r4,r4,1; cmplw r4,r5; bge exit; bctr` back to an entry-less head, so
// recomp::jump_indirect leaves without a site (as here). Progress every lap; the third lap takes the
// exit to LR. Comparing r1/LR/MSR used to take every lap for a stall.
void counting_loop(Cpu& c) {
    ++c.gpr[4];
    if (c.gpr[4] >= c.gpr[5]) {
        c.pc = c.lr;
        return;
    }
    c.leave_to_dispatcher(c.pc);
}
// A host stop at the body's own entry, after work of its own or with none: honoured as the stop it is,
// never mistaken for that stall.
template<bool Work> void host_stop(Cpu& c) {
    if constexpr (Work) { ++c.gpr[3]; ++c.gpr[4]; }
    c.request_stop(StopReason::HostRequest, "native host stop");
    c.leave_to_dispatcher(c.pc, FallbackReason::Yield, c.pc);
}

// Production never interprets: an address it cannot run natively stops the machine right there, after
// `dispatches` native call trees (one for a body that hands back at its own entry) and with nothing else
// changed.
std::unique_ptr<Fixture> missing(RecompiledCode* code, u32 entry, u64 dispatches = 0u) {
    auto f = std::make_unique<Fixture>(code, entry);
    State before(*f);
    before.dispatched(dispatches);
    f->cpu.run();
    native_fault(*f, entry); before.check(*f); f->no_fetch();
    return f;
}
void indirect(bool call) {
    ThunkTable table(call ? call_missing : jump_missing);
    auto f = std::make_unique<Fixture>(&table.code, kThunk);
    State expected(*f);
    expected.dispatched(1u);  // the native branch, and nothing after it
    if (call) expected.lr = kThunk + 4u;
    f->cpu.run();
    native_fault(*f, kMissing); expected.check(*f); f->no_fetch();
    require(f->cpu.fallback_total(FallbackReason::NotRecompiled) == 1u &&
                f->cpu.native_transfer_leaves == (call ? 0u : 1u),
            "a missing indirect target must hand back exactly once, as NotRecompiled");
}
void untranslated_hle(RecompiledCode& code, bool served) {
    auto f = std::make_unique<Fixture>(&code);
    State before(*f);
    unsigned hits = 0;
    f->cpu.install_hle(kMissing, [&](Cpu& c) {
        ++hits; ++c.gpr[7]; return served;
    }, "untranslated hook");
    f->cpu.run();
    require(hits == 0u, "untranslated HLE was invoked as hidden fallback");
    require(f->cpu.dispatch_blocked_hle == 1u, "the dispatcher must refuse a hooked address with no translated code");
    native_fault(*f, kMissing); before.check(*f); f->no_fetch();
}
void native_control(RecompiledCode::Fn fn, u64 dispatches, u32 stack_moved, u32 work, const char* detail) {
    ThunkTable table(fn);
    auto f = std::make_unique<Fixture>(&table.code, kThunk);
    State expected(*f);
    expected.dispatched(dispatches);
    expected.gpr[1] += stack_moved;
    expected.gpr[3] += work;
    expected.gpr[4] += work;
    g_yields = 0;
    f->cpu.run();
    require(f->cpu.stop().reason == StopReason::HostRequest && f->cpu.pc == kThunk, "native yield/host stop was replaced");
    require(f->cpu.stop().detail == detail, "host stop diagnostic was replaced");
    require(f->cpu.fallback_total(FallbackReason::Yield) == dispatches, "every dispatch must hand back as one yield");
    expected.check(*f); f->no_fetch();
}

// The production dispatcher has no budget: a generated function that returns to LR is followed by a
// dispatch at LR, and with no code there the machine stops - after exactly one native call tree.
void generated(RecompiledCode& code, u32 entry) {
    auto f = std::make_unique<Fixture>(&code, entry);
    State expected(*f);
    f->cpu.set_verify_recompiled(false);
    expected.check(*f);
    require(!f->cpu.stopped() && f->cpu.pc == entry, "disabling verification changed execution state");
    // count: the function's instructions from this entry (each case executes twelve of them in all).
    const u32 offset = entry & 0xFu, count = 3u - offset / 4u;
    if (offset != 0u)
        require(code.lookup(entry) == nullptr && code.find_range(entry) != nullptr, "fixture must use range entry");
    for (u32 i = 0; i < 12u / count; ++i) {
        f->cpu.clear_stop();
        f->cpu.set_entry(entry);
        f->cpu.run();
        if (offset == 0u) ++expected.gpr[3];
        if (offset <= 4u) ++expected.gpr[4];
        expected.dispatched(1u);
        require(f->cpu.pc == f->cpu.lr, "generated function failed to return");
        native_fault(*f, f->cpu.lr);
        require(f->cpu.dispatch_entered_mid_function == (offset != 0u ? i + 1u : 0u),
                "an interior entry must run its translated range");
        expected.check(*f);
    }
    f->no_fetch();
}
void call_handoff(RecompiledCode& code, bool decrementer, u32 entry) {
    auto f = std::make_unique<Fixture>(&code, entry);
    f->cpu.lr = kMissing;
    f->cpu.set_msr(f->cpu.msr() | (kMsrEE));
    // The decrementer's 0 -> 1 edge is a game-clock event the scheduler delivers at a wait point; this
    // Cpu-only fixture delivers it directly.
    if (decrementer) { f->cpu.set_decrementer(0u); f->cpu.latch_decrementer(); }
    else f->cpu.set_external_interrupt(true);
    State issued(*f);
    State expected(*f);
    expected.dispatched(1u);  // the callee's call tree; nothing native runs at the handler
    if (entry == kEntry) ++expected.gpr[3];
    if (entry <= kEntry + 4u) ++expected.gpr[4];
    expected.take_exception(decrementer ? 8u : 4u, decrementer ? kDecrementerHandler : kExternalHandler,
                            kMissing);
    f->cpu.handoff_native_call(entry, kThunk);
    require(f->cpu.pc == entry && f->cpu.recompiled_calls == 0u && f->cpu.gpr[3] == issued.gpr[3] &&
                f->cpu.gpr[4] == issued.gpr[4], "handoff ran its callee on the caller's stack");
    issued.check(*f);
    post_host_stop(f->cpu, "host stop before the pending call");
    f->cpu.run();
    require(f->cpu.stop().reason == StopReason::HostRequest && f->cpu.pc == entry,
            "a run that made no dispatch entered the pending call");
    issued.check(*f);
    f->cpu.clear_stop();
    f->cpu.run();
    require(saved_srr0(*f) == kMissing, "pending IRQ was recognized before the native callee");
    native_fault(*f, decrementer ? kDecrementerHandler : kExternalHandler);
    expected.check(*f);
    f->no_fetch();
}

// The issuing side of a native call handoff. The external line rises inside the issuing body (a condition
// that becomes deliverable while native code runs - the hardware tick that raised it here went with the
// instruction clock), the body hands the call to the dispatcher, and a host stop lands right after it, so
// the run ends at the boundary where the callee would have been entered.
void handoff_bridge(Cpu& c) {
    c.set_external_interrupt(true);
    c.lr = kMissing;
    c.handoff_native_call(kEntry, kThunk);
    c.request_stop(StopReason::HostRequest, "stop between handoff and entry");
}

// ---- R12 (2026-09-28): LK=0 transfers are published; the waiting frame runs them ----
//
// Hand-written bodies in the shape tools/ppcrecomp emits: an LK=0 transfer to another record is
// `c.transfer_to(&target); return;` (or recomp::jump_indirect), and a linked call is
// `fn(c); if (c.exiting() && c.finish_transfers()) return; if (c.pc != link) {...}`.
namespace r12 {
constexpr u32 kRingA = 0x80008400u, kRingB = 0x80008410u, kRingC = 0x80008420u;
constexpr u32 kCaller = 0x80008430u, kCallerLink = 0x80008438u, kChainA = 0x80008440u, kChainB = 0x80008450u;
constexpr u32 kIssuer = 0x80008460u, kTarget = 0x80008470u, kLast = 0x80008480u;

std::uintptr_t g_low = ~std::uintptr_t{0}, g_high = 0;
u64 g_caller_continued = 0, g_target_ran = 0, g_target_saw_exit = 0, g_never_ran = 0;
// The one guest instruction each body stands for, counted here: native code has no instruction count, so
// this is what shows every body ran exactly as often as the transfers to it say.
u64 g_executed = 0;
bool g_issuer_stops = false;

GEKKO_NOINLINE void note_depth() {
    const auto sp = reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
    if (sp < g_low) g_low = sp;
    if (sp > g_high) g_high = sp;
}
void last(Cpu& c) {
    if (c.try_hle(kLast)) return;
    c.request_stop(StopReason::HostRequest, "r12 done");
    c.leave_to_dispatcher(kLast, FallbackReason::Yield, kLast);
}
void ring_b(Cpu& c);
void ring_c(Cpu& c);
void ring_a(Cpu& c) {  // counts laps in r3; `b` to B, or to the end
    if (c.try_hle(kRingA)) return;
    note_depth();
    ++g_executed;
    if (--c.gpr[3] == 0u) { c.transfer_to(&last); return; }
    c.transfer_to(&ring_b);
}
void ring_b(Cpu& c) {  // bctr to C: the jump_indirect path
    if (c.try_hle(kRingB)) return;
    note_depth();
    ++g_executed;
    recomp::jump_indirect(c, kRingC);
}
void ring_c(Cpu& c) {  // fall-through / `b` back to A
    if (c.try_hle(kRingC)) return;
    note_depth();
    ++g_executed;
    c.transfer_to(&ring_a);
}
void chain_b(Cpu& c) {  // blr to the caller's link
    if (c.try_hle(kChainB)) return;
    ++g_executed;
    c.pc = c.lr;
}
void chain_a(Cpu& c) {  // `b` to chain_b: an LK=0 transfer inside a linked callee
    if (c.try_hle(kChainA)) return;
    ++g_executed;
    c.transfer_to(&chain_b);
}
void caller(Cpu& c) {  // bl chain_a, then its own continuation
    if (c.try_hle(kCaller)) return;
    ++g_executed;
    c.lr = kCallerLink;
    chain_a(c);
    if (c.exiting() && c.finish_transfers()) return;
    if (c.pc != kCallerLink) { recomp::resume_elsewhere(c, kCaller); return; }
    ++g_caller_continued;
    ++g_executed;
    c.transfer_to(&last);
}
void never(Cpu& c) { ++g_never_ran; (void)c; }
void target(Cpu& c) {  // one instruction, then its own recognition point (a post-call style check)
    if (c.try_hle(kTarget)) return;
    ++g_target_ran;
    ++g_executed;
    if (c.exiting()) {
        ++g_target_saw_exit;
        c.request_stop(StopReason::HostRequest, "r12 target");
        c.leave_to_dispatcher(kTarget + 4u, FallbackReason::Yield, kTarget);
        return;
    }
    c.transfer_to(&last);
}
void issuer(Cpu& c) {  // makes an interrupt deliverable (or a stop) inside its block, then `b` target
    if (c.try_hle(kIssuer)) return;
    ++g_executed;
    if (g_issuer_stops) c.request_stop(StopReason::HostRequest, "r12 stop before transfer");
    else c.set_external_interrupt(true);
    c.transfer_to(&target);
}

const RecompiledCode::Entry kEntries[] = {
    {kRingA, 16u, ring_a, "r12 ring A"},     {kRingB, 16u, ring_b, "r12 ring B"},
    {kRingC, 16u, ring_c, "r12 ring C"},     {kCaller, 16u, caller, "r12 caller"},
    {kChainA, 16u, chain_a, "r12 chain A"},  {kChainB, 16u, chain_b, "r12 chain B"},
    {kIssuer, 16u, issuer, "r12 issuer"},    {kTarget, 16u, target, "r12 target"},
    {kLast, 16u, last, "r12 last"},
};

struct Machine {
    GuestMemory bus;
    Cpu cpu{bus};
    RecompiledCode code;
    explicit Machine(u32 entry) {
        code.install(kEntries, sizeof(kEntries) / sizeof(kEntries[0]));
        cpu.reset();
        cpu.attach_recompiled(&code);
        cpu.set_entry(entry);
    }
};

// A cycle of LK=0 transfers (b, bctr through jump_indirect, b) costs one body frame at a time: the host
// stack span over every body entry is the same after 200,000 laps as after 1,000.
void cycle_keeps_host_stack_flat() {
    auto spread = [](u32 laps, u64& driven, u64& calls, u64& executed) {
        auto m = std::make_unique<Machine>(kRingA);
        m->cpu.gpr[3] = laps;
        g_low = ~std::uintptr_t{0};
        g_high = 0;
        g_executed = 0;
        m->cpu.run();
        require(m->cpu.stop().reason == StopReason::HostRequest && m->cpu.gpr[3] == 0u, "ring did not finish");
        require(m->cpu.recompiled_calls == 1u, "r12: the whole cycle must run inside one native dispatch");
        executed = g_executed;
        driven = m->cpu.native_transfers_driven;
        calls = m->cpu.native_transfer_calls;
        return g_high - g_low;
    };
    u64 d1 = 0, c1 = 0, e1 = 0, d2 = 0, c2 = 0, e2 = 0;
    const std::uintptr_t short_span = spread(1000u, d1, c1, e1);
    const std::uintptr_t long_span = spread(200000u, d2, c2, e2);
    require(long_span == short_span, "r12: the host stack span grew with the number of transfer laps");
    require(short_span < 512u, "r12: bodies entered by the driver must not nest");
    // A runs `laps` times, B and C `laps - 1` times (the last lap leaves from A): each body runs its one
    // instruction and publishes one transfer (A's last one goes to `last`); only B uses jump_indirect.
    require(e1 == 3u * 1000u - 2u && e2 == 3u * 200000u - 2u, "r12: every body runs exactly once per transfer to it");
    require(d1 == 3u * 1000u - 2u && d2 == 3u * 200000u - 2u, "r12: every published transfer must run exactly once");
    require(c1 == 1000u - 1u && c2 == 200000u - 1u, "r12: jump_indirect keeps counting its native transfers");
}

// A callee's transfer chain runs in the linked caller's frame, which then continues at its link (the
// 2026-09-22 trampoline unwound this frame instead).
void linked_caller_keeps_its_continuation() {
    auto m = std::make_unique<Machine>(kCaller);
    g_caller_continued = 0;
    g_executed = 0;
    m->cpu.run();
    require(g_caller_continued == 1u, "r12: the caller's continuation did not run in its own frame");
    require(g_executed == 4u && m->cpu.stop().reason == StopReason::HostRequest &&
                m->cpu.recompiled_calls == 1u,
            "r12: caller, chain A, chain B and the continuation each run once, in one dispatch");
    require(m->cpu.native_transfers_driven == 2u, "r12: chain_a -> chain_b and caller -> last");
}

// A deliverable interrupt latched inside the issuing block is NOT recognized between the transfer and its
// target: the target runs its first instruction and meets the interrupt at its own recognition point, as it
// did when the transfer was a tail call. A pending stop does not skip the target either.
void no_recognition_point_before_the_target(bool stop) {
    auto m = std::make_unique<Machine>(kIssuer);
    m->cpu.set_msr(m->cpu.msr() | kMsrEE);
    g_target_ran = 0;
    g_target_saw_exit = 0;
    g_executed = 0;
    g_issuer_stops = stop;
    m->cpu.run();
    g_issuer_stops = false;
    require(g_target_ran == 1u, "r12: the published target did not run before the pending condition");
    require(g_target_saw_exit == 1u, "r12: the target's own recognition point must see the condition");
    require(g_executed == 2u && m->cpu.recompiled_calls == 1u,
            "r12: issuer and target each run their one instruction, in one dispatch");
    if (!stop) {
        require(m->cpu.interrupt_pending(), "r12: the interrupt must still be pending, not dropped");
    }
}

// leave_to_dispatcher supersedes a published transfer (a body never publishes and then leaves; host code
// that does gets the dispatcher, not the stale entry).
void leave_supersedes_a_published_transfer() {
    auto m = std::make_unique<Machine>(kLast);
    g_never_ran = 0;
    m->cpu.transfer_to(&never);
    m->cpu.leave_to_dispatcher(kLast);
    require(m->cpu.finish_transfers(), "r12: a leave must still read as exiting");
    require(g_never_ran == 0u && m->cpu.native_transfers_driven == 0u, "r12: a transfer survived a leave");
}
}  // namespace r12

// A native call intent crosses run boundaries: the run that issued it ends with it pending, a run that
// makes no dispatch neither enters nor discards it, and the next dispatch enters the callee ahead of the
// deliverable interrupt, which is then taken at the callee's return address.
void split_call_handoff(bool host_pause) {
    std::vector<RecompiledCode::Entry> entries(recomp::kFunctions, recomp::kFunctions + recomp::kFunctionCount);
    entries.push_back({kThunk, 4u, handoff_bridge, "native-call-handoff"});
    RecompiledCode code;
    code.install(entries.data(), entries.size());
    code.install_ranges(recomp::kRanges, recomp::kRangeCount);
    auto f = std::make_unique<Fixture>(&code, kThunk);
    f->cpu.set_msr(f->cpu.msr() | (kMsrEE));
    State expected(*f);
    expected.dispatched(2u);  // the bridge, then the callee
    ++expected.gpr[3]; ++expected.gpr[4];
    expected.lr = kMissing;
    expected.take_exception(4u, kExternalHandler, kMissing);
    const u32 r3 = f->cpu.gpr[3], r4 = f->cpu.gpr[4];
    f->cpu.run();
    require(f->cpu.stop().reason == StopReason::HostRequest && f->cpu.pc == kEntry &&
                f->cpu.recompiled_calls == 1u && f->cpu.interrupt_pending(),
            "handoff did not cross the run boundary");
    require(f->cpu.gpr[3] == r3 && f->cpu.gpr[4] == r4, "callee ran before its own dispatcher iteration");
    f->cpu.clear_stop();
    post_host_stop(f->cpu, "host stop between handoff and entry");
    f->cpu.run();
    require(f->cpu.stop().reason == StopReason::HostRequest && f->cpu.pc == kEntry &&
                f->cpu.recompiled_calls == 1u && f->cpu.gpr[3] == r3,
            "a run that made no dispatch consumed the handoff");
    f->cpu.clear_stop();
    if (host_pause) {
        f->cpu.request_stop(StopReason::HostRequest, "pause between handoff and entry");
        f->cpu.run();
        require(f->cpu.stop().detail == "pause between handoff and entry" && f->cpu.recompiled_calls == 1u &&
                    f->cpu.pc == kEntry, "stopped CPU consumed the handoff");
        f->cpu.clear_stop();
    }
    f->cpu.run();
    require(saved_gpr(*f, 3u) == r3 + 1u && saved_gpr(*f, 4u) == r4 + 1u, "callee did not execute once");
    require(f->cpu.external_interrupt() && saved_srr0(*f) == kMissing,
            "handoff suppressed the interrupt condition, or its exemption leaked past the callee return");
    native_fault(*f, kExternalHandler);
    expected.check(*f);  // the run ledger: two native dispatches, nothing interpreted, no game time
    f->no_fetch();
}

void cancelled_call_handoff(RecompiledCode& code, unsigned change) {
    auto f = std::make_unique<Fixture>(&code, kEntry);
    std::vector<u8> saved;
    f->cpu.save_state(saved);
    f->cpu.handoff_native_call(kEntry, kThunk);
    switch (change) {
    case 0: f->cpu.leave_to_dispatcher(kEntry, FallbackReason::Yield, kThunk); break;
    case 1: f->cpu.set_entry(kEntry); break;
    case 2: f->cpu.attach_recompiled(&code); break;
    case 3: f->cpu.reset(); f->cpu.pc = kEntry; break;
    case 4: {
        const u8* cursor = saved.data();
        require(f->cpu.load_state(cursor, saved.data() + saved.size(), 11u), "CPU state load failed");
        break;
    }
    case 5: f->cpu.pc = kEntry + 4u; break;
    default: throw std::runtime_error("unknown handoff cancellation case");
    }
    f->cpu.set_msr(f->cpu.msr() | (kMsrEE));
    f->cpu.set_external_interrupt(true);
    const u32 expected_pc = f->cpu.pc;
    const u64 dispatches = f->cpu.recompiled_calls;
    const u32 r3 = f->cpu.gpr[3], r4 = f->cpu.gpr[4];
    f->cpu.run();
    require(f->cpu.recompiled_calls == dispatches && saved_gpr(*f, 3u) == r3 && saved_gpr(*f, 4u) == r4,
            "stale call intent bypassed ordinary R1");
    require(saved_srr0(*f) == expected_pc, "ordinary R1 captured the wrong resume PC");
    native_fault(*f, kExternalHandler);
}

void handoff_fp_fault(RecompiledCode& code) {
    auto f = std::make_unique<Fixture>(&code, kSemFpSinglesAddress);
    f->cpu.set_msr((f->cpu.msr() | kMsrEE) & ~kMsrFP);
    f->cpu.set_external_interrupt(true);
    State expected(*f);
    expected.dispatched(1u);  // the callee, faulting on its first instruction
    expected.take_exception(7u, kFpHandler, kSemFpSinglesAddress);
    f->cpu.handoff_native_call(kSemFpSinglesAddress, kThunk);
    f->cpu.run();
    require(saved_srr0(*f) == kSemFpSinglesAddress, "callee FP fault was displaced by the pre-call IRQ");
    native_fault(*f, kFpHandler);
    expected.check(*f);
    require(f->cpu.external_interrupt(), "FP fault discarded the pending external condition");
}

void fallback_call_handoff(RecompiledCode& code, u32 target) {
    require(code.lookup(target) == nullptr, "fallback fixture unexpectedly has a declared native entry");
    for (bool handoff : {false, true}) {
        auto f = std::make_unique<Fixture>(&code, kEntry);
        f->cpu.set_msr(f->cpu.msr() | (kMsrEE));
        f->cpu.set_external_interrupt(true);
        const u32 r3 = f->cpu.gpr[3], r4 = f->cpu.gpr[4];
        if (handoff) f->cpu.handoff_native_call(target, kThunk);
        else recomp::call_indirect(f->cpu, target, f->cpu.lr);
        f->cpu.run();
        require(saved_srr0(*f) == target && f->cpu.recompiled_calls == 0u && saved_gpr(*f, 3u) == r3 &&
                    saved_gpr(*f, 4u) == r4,
                "unresolved call target lost the reference's dispatcher recognition point");
        native_fault(*f, kExternalHandler);
        f->no_fetch();
    }
}

void callee_recognition_point(RecompiledCode& code, bool back_edge) {
    const u32 entry = back_edge ? kPollStableAddress : kIrqMtmsrDisableAddress;
    auto f = std::make_unique<Fixture>(&code, entry);
    f->cpu.gpr[4] = 0x80009204u;
    f->cpu.set_msr(f->cpu.msr() | (kMsrEE));
    f->cpu.set_external_interrupt(true);
    const u32 msr = f->cpu.msr(), r5 = f->cpu.gpr[5];
    f->cpu.handoff_native_call(entry, kThunk);
    f->cpu.run();
    // What ran before the recognition point is what the callee's own instructions left (saved in the
    // OSContext by the OS entry): lwz r3 (0 from 0x80009204), cmplwi (CR0 = EQ|SO) and the taken beq; or
    // mfmsr r3 and the rlwinm clearing EE in r4, with the mtmsr and the addi r5 after it not run.
    if (back_edge) {
        require(saved_gpr(*f, 3u) == 0u && (saved_cr(*f) >> 28) == 3u,
                "handoff changed the callee's own recognition point");
    } else {
        require(saved_gpr(*f, 3u) == msr && saved_gpr(*f, 4u) == (msr & ~kMsrEE) && saved_gpr(*f, 5u) == r5,
                "handoff changed the callee's own recognition point");
    }
    require(f->cpu.recompiled_calls == 1u, "the callee must run as one native dispatch");
    require(saved_srr0(*f) == (back_edge ? entry : entry + 8u) && (saved_srr1(*f) & kMsrEE) != 0u,
            "interrupt moved beyond the callee's recognition instruction");
    native_fault(*f, kExternalHandler);
}

void hle_call_handoff(RecompiledCode& code, bool zero_progress_yield) {
    auto f = std::make_unique<Fixture>(&code, kEntry);
    unsigned calls = 0;
    f->cpu.lr = kMissing;
    f->cpu.set_msr(f->cpu.msr() | (kMsrEE));
    f->cpu.set_external_interrupt(true);
    f->cpu.install_hle(kEntry, [&](Cpu& c) {
        require(++calls == 1u, "callee hook was invoked more than once");
        require(c.interrupt_pending() && c.exiting(), "handoff masked the native exit signal");
        if (zero_progress_yield) c.leave_to_dispatcher(kEntry, FallbackReason::Yield, kEntry);
        return true;
    }, "handoff served callee");
    const u32 r3 = f->cpu.gpr[3], r4 = f->cpu.gpr[4];
    f->cpu.handoff_native_call(kEntry, kThunk);
    require(calls == 0u, "callee hook was invoked on the issuing host stack");
    f->cpu.run();
    require(calls == 1u && f->cpu.recompiled_calls == 1u, "handoff did not preserve the callee hook boundary");
    native_fault(*f, kExternalHandler);
    require(saved_srr0(*f) == (zero_progress_yield ? kEntry : kMissing), "callee hook resume point changed");
    // The served call is the hook's: its served-call ledger has it, and the translated body (r3, r4) never
    // ran - natively or otherwise.
    require(f->cpu.hle_entries().size() == 1u && f->cpu.hle_entries().front().hits == 1u &&
                saved_gpr(*f, 3u) == r3 && saved_gpr(*f, 4u) == r4 && f->cpu.interpreted_instructions() == 0u,
            "callee hook classification changed");
    f->no_fetch();
}

void handoff_is_cpu_owned(RecompiledCode& code) {
    auto first = std::make_unique<Fixture>(&code, kEntry);
    auto second = std::make_unique<Fixture>(&code, kEntry);
    for (auto* f : {first.get(), second.get()}) {
        f->cpu.lr = kMissing;
        f->cpu.set_msr(f->cpu.msr() | (kMsrEE));
        f->cpu.set_external_interrupt(true);
    }
    const u32 r3 = first->cpu.gpr[3];
    first->cpu.handoff_native_call(kEntry, kThunk);
    second->cpu.run();
    require(second->cpu.recompiled_calls == 0u && saved_gpr(*second, 3u) == r3 && saved_srr0(*second) == kEntry,
            "call intent leaked to a different CPU");
    first->cpu.run();
    require(first->cpu.recompiled_calls == 1u && saved_gpr(*first, 3u) == r3 + 1u &&
                saved_srr0(*first) == kMissing,
            "different CPU consumed the call intent");
    native_fault(*first, kExternalHandler);
    native_fault(*second, kExternalHandler);
}

void pending_exception(RecompiledCode& code, bool decrementer) {
    auto f = std::make_unique<Fixture>(&code, kEntry);
    f->cpu.set_msr(f->cpu.msr() | (kMsrEE));
    // The decrementer edge as the scheduler delivers it at a wait point (see call_handoff).
    if (decrementer) { f->cpu.set_decrementer(0u); f->cpu.latch_decrementer(); }
    else f->cpu.set_external_interrupt(true);
    State expected(*f);
    expected.take_exception(decrementer ? 8u : 4u, decrementer ? kDecrementerHandler : kExternalHandler, kEntry);
    f->cpu.run();
    native_fault(*f, decrementer ? kDecrementerHandler : kExternalHandler); expected.check(*f); f->no_fetch();
}
}

int main() {
    RecompiledCode code, empty;
    code.install(recomp::kFunctions, recomp::kFunctionCount);
    code.install_ranges(recomp::kRanges, recomp::kRangeCount);
    empty.install(nullptr, 0u);
    unsigned failures = 0, cases = 0;
    auto test = [&](const std::string& name, auto body) {
        ++cases;
        const u64 before = g_non_native_ppc_executions.load();
        try {
            body();
            require(g_non_native_ppc_executions.load() == before, "non-native PPC execution detected");
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << " (non-native +"
                      << g_non_native_ppc_executions.load() - before << ")\n";
        }
    };
    test("no table", [&] { missing(nullptr, kEntry); });
    test("empty table", [&] { missing(&empty, kMissing); });
    test("missing call_indirect target", [] { indirect(true); });
    test("missing jump_indirect target", [] { indirect(false); });
    test("r12: LK=0 transfer cycle keeps the host stack flat", [] { r12::cycle_keeps_host_stack_flat(); });
    test("r12: linked caller keeps its continuation", [] { r12::linked_caller_keeps_its_continuation(); });
    test("r12: no recognition point before the target (interrupt)",
         [] { r12::no_recognition_point_before_the_target(false); });
    test("r12: no recognition point before the target (stop)",
         [] { r12::no_recognition_point_before_the_target(true); });
    test("r12: leave supersedes a published transfer", [] { r12::leave_supersedes_a_published_transfer(); });
    for (const u32 handler : {kExternalHandler, kFpHandler, kDecrementerHandler, kMissing})
        require(code.lookup(handler) == nullptr && code.find_range(handler) == nullptr,
                "the fixture's handlers and missing target must have no translated code");
    test("unmapped entry below the image", [&] { missing(&code, 0x02000000u); });
    test("unmapped entry above the image", [&] { missing(&code, 0x90000000u); });
    test("device fixture entry", [&] { missing(&code, kDevicePage); });
    test("untranslated served HLE", [&] { untranslated_hle(code, true); });
    test("untranslated declined HLE", [&] { untranslated_hle(code, false); });
    test("native no-progress exit", [] { ThunkTable table(no_progress); missing(&table.code, kThunk, 1u); });
    test("native zero-progress yield", [] {
        ThunkTable table(stalled_yield);
        const auto f = missing(&table.code, kThunk, 1u);
        require(f->cpu.fallback_total(FallbackReason::Yield) == 1u, "the stall must be the body's own yield");
    });
    test("verification rejected", [&] {
        auto f = std::make_unique<Fixture>(&code, kEntry);
        State before(*f);
        f->cpu.set_verify_recompiled(true);
        f->cpu.run();
        require(f->cpu.stop().reason == StopReason::ExecutionFault && !f->cpu.stop().detail.empty(),
                "verification must fail loudly without an oracle");
        require(f->cpu.pc == kEntry && f->cpu.stop().address == kEntry, "verification fault PC mismatch");
        before.check(*f); f->no_fetch();  // no native dispatch, no replay
    });
    // A run whose first boundary serves a host thread's stop makes no dispatch: nothing runs and the
    // deliverable interrupt is not taken (it was the zero instruction budget's contract).
    for (RecompiledCode* table : {static_cast<RecompiledCode*>(nullptr), &code})
        test(table ? "generated: host stop before the first dispatch" : "missing: host stop before the first dispatch", [&] {
            auto f = std::make_unique<Fixture>(table, kEntry);
            f->cpu.set_msr(f->cpu.msr() | (kMsrEE)); f->cpu.set_external_interrupt(true);
            State before(*f);
            post_host_stop(f->cpu, "host stop before dispatch");
            f->cpu.run();
            require(f->cpu.stop().reason == StopReason::HostRequest &&
                        f->cpu.stop().detail.find("host stop before dispatch; guest pc=0x80008000") == 0u,
                    "the host stop was not served at the first boundary, naming the guest location");
            require(f->cpu.pc == kEntry && f->cpu.npc == kEntry + 4u && f->cpu.interrupt_pending(),
                    "a run that made no dispatch delivered an exception");
            before.check(*f); f->no_fetch();
        });
    test("native yield with progress", [] { native_control(yielding, 12u, 12u * 0x100u, 0u, "native yield"); });
    test("native loop through the dispatcher is progress", [] {
        ThunkTable table(counting_loop);
        auto f = std::make_unique<Fixture>(&table.code, kThunk);
        f->cpu.gpr[4] = 0x32C08A91u;
        f->cpu.gpr[5] = 0x32C08A94u;
        f->cpu.lr = kMissing;
        State expected(*f);
        expected.dispatched(3u);
        expected.gpr[4] = 0x32C08A94u;
        expected.lr = kMissing;
        f->cpu.run();
        native_fault(*f, kMissing); expected.check(*f); f->no_fetch();
        require(f->cpu.fallback_total(FallbackReason::NotRecompiled) == 2u,
                "two laps must hand back to the dispatcher and the third reach the exit");
    });
    test("native host stop after work", [] { native_control(host_stop<true>, 1u, 0u, 1u, "native host stop"); });
    test("native host stop without work", [] { native_control(host_stop<false>, 1u, 0u, 0u, "native host stop"); });
    test("existing host stop", [] {
        auto f = std::make_unique<Fixture>(nullptr);
        f->cpu.request_stop(StopReason::HostRequest, "existing host stop");
        State before(*f); f->cpu.set_verify_recompiled(false);
        f->cpu.run();
        require(f->cpu.stop().reason == StopReason::HostRequest &&
                    f->cpu.stop().detail == "existing host stop", "existing host stop was replaced");
        before.check(*f); f->no_fetch();
    });
    test("pending external interrupt", [&] { pending_exception(code, false); });
    test("pending decrementer", [&] { pending_exception(code, true); });
    test("native call handoff, pending external", [&] { call_handoff(code, false, kEntry); });
    test("native call handoff, pending decrementer", [&] { call_handoff(code, true, kEntry); });
    test("range-only handoff retains dispatcher R1", [&] { fallback_call_handoff(code, kEntry + 4u); });
    test("missing handoff retains dispatcher R1", [&] { fallback_call_handoff(code, kMissing); });
    test("native call handoff preserves mtmsr boundary", [&] { callee_recognition_point(code, false); });
    test("native call handoff preserves back-edge boundary", [&] { callee_recognition_point(code, true); });
    test("native call handoff, served hook", [&] { hle_call_handoff(code, false); });
    test("native call handoff, zero-progress callee yield", [&] { hle_call_handoff(code, true); });
    test("native call handoff belongs to one CPU", [&] { handoff_is_cpu_owned(code); });
    test("native call handoff across a run boundary", [] { split_call_handoff(false); });
    test("native call handoff across host pause", [] { split_call_handoff(true); });
    for (unsigned change = 0; change < 6u; ++change)
        test("native call handoff cancellation " + std::to_string(change), [&] { cancelled_call_handoff(code, change); });
    test("native call handoff preserves FP fault precedence", [&] { handoff_fp_fault(code); });
    test("generated entry, verification disabled", [&] { generated(code, kEntry); });
    test("generated interior range", [&] { generated(code, kEntry + 4u); });
    test("non-native execution total", [] {
        require(g_non_native_ppc_executions.load() == 0u, "non-native PPC counter must remain zero");
    });
    std::cout << cases << " cases, " << failures << " failures\n";
    return failures == 0u ? 0 : 1;
}
