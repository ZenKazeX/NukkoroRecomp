// Generated polling loops (tools/ppcrecomp on the synthetic fallback image)
// against the reference interpreter.
//
// The six fixture shapes are the RAM polls the deleted coalesce_ram_poll either
// matched or had to refuse: `lwz; cmplwi; beq self` with a stable base, an
// aliased base, an absolute address, a compare with one, and hot entries into
// the compare and into the branch, each entered at its root and through its
// range at the loop's own instructions. Under native game time
// (docs/native-time-20261005/DESIGN.md) nothing counts instructions and guest
// code runs in zero game time; a loop waits only where the generator was told
// it may (extract/wait-points.txt: Cpu::idle_wait / Cpu::poll_point on the
// back edge). This image is generated without a wait-point list, so every loop
// here is UNLISTED: its taken back edge publishes pc and polls
// needs_dispatch() (recognition point R3, cpu.h) and does nothing else. The
// contracts, each against the interpreter run of the original words:
//   * a loop its own reads end returns natively with the reference's exact
//     result, never waits, and leaves the game clock as it found it;
//   * a loop only an event can end hands back at its head (FallbackReason::
//     Yield from the back-edge site) on its first taken back edge once an
//     interrupt is deliverable - the interpreter recognises it at the same
//     boundary - or a host stop is posted, and completes identically once the
//     event's effect released the flag: the decrementer handler's store, or
//     the host's write at the wait point that follows the stop.
// The listed form of this shape - the SelectThread idle loop 0x803788FC,
// emitted with c.idle_wait() - runs as real generated game code in
// compiler_body_tests, which links the game shards (this test links the
// fixture's own table, and the fixture is generated without a list).
#include "gekko/cpu.h"
#include "gekko/cpu_diag.h"
#include "gekko/os_exception_vector.h"
#include "gekko/recomp.h"
#include "fallback_fixture.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gekko;

namespace {
constexpr u32 kFlag = 0x10000u;
constexpr u32 kMarker = 0x18000u;
constexpr u32 kReturn = 0x80009000u;
constexpr u32 kCrTail = 0x013579BDu;
constexpr u32 kDecrementer = 0x01234567u;
constexpr u64 kStepCap = 1000u;

// The decrementer handler. The native OS exception entry
// (os_exception_entry.cpp) saves r3-r5, CR, LR, CTR, XER, SRR0 and SRR1 in the
// current OSContext and enters the handler __OSExceptionTable names for index
// 8 with r3 = 8 and r4 = the context. The synthetic image has no native code
// at the handler, so both machines interpret it: count the delivery in the
// marker word, release the polled flag, and return to the interrupted loop
// head the way OSLoadContext does (CR, SRR0, SRR1 and r3-r5 from the context,
// then rfi). r28/r29/r30 are its operands (Fixture).
constexpr u32 kHandler = 0x80009400u;
constexpr u32 kContext = 0x8000A000u;   // the current OSContext (low memory 0xD4)
constexpr u32 kHandlerWords[] = {
    0x837C0000u,  // lwz  r27,0(r28)   marker
    0x3B7B0001u,  // addi r27,r27,1
    0x937C0000u,  // stw  r27,0(r28)
    0x93BE0000u,  // stw  r29,0(r30)   the polled flag = its release value
    0x83640080u,  // lwz  r27,0x80(r4)  CR
    0x7F6FF120u,  // mtcrf 0xFF,r27
    0x83640198u,  // lwz  r27,0x198(r4) SRR0
    0x7F7A03A6u,  // mtsrr0 r27
    0x8364019Cu,  // lwz  r27,0x19C(r4) SRR1
    0x7F7B03A6u,  // mtsrr1 r27
    0x8064000Cu,  // lwz  r3,0x0C(r4)
    0x80A40014u,  // lwz  r5,0x14(r4)
    0x80840010u,  // lwz  r4,0x10(r4)
    0x4C000064u,  // rfi
};
constexpr u64 kHandlerInstructions = sizeof(kHandlerWords) / sizeof(kHandlerWords[0]);

// Every mapped region's committed bytes, in region order.
std::vector<u8> window_snapshot(const GuestMemory& memory) {
    std::vector<u8> bytes;
    for (const auto& region : memory.regions()) {
        const auto part = memory.window_bytes(region.base, region.base + region.committed);
        bytes.insert(bytes.end(), part.begin(), part.end());
    }
    return bytes;
}

void require(bool ok, const std::string& detail) {
    if (!ok) throw std::runtime_error(detail);
}

std::string hex(u32 value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

struct Program {
    u32 address;
    const unsigned* words;
    std::size_t count;
    u32 flag;
    u32 result_register;
    u32 exit_value;
    u32 exit_cr0;
};

template <std::size_t N>
constexpr Program program(u32 address, const unsigned (&words)[N], u32 flag = kFlag,
                          u32 result_register = 3u, u32 exit_value = 1u, u32 exit_cr0 = 5u) {
    return {address, words, N, flag, result_register, exit_value, exit_cr0};
}

constexpr auto kAlias = program(kPollAliasAddress, kPollAliasWords);
constexpr auto kStable = program(kPollStableAddress, kPollStableWords);
constexpr auto kHotCompare = program(kPollHotCompareAddress, kPollHotCompareWords);
constexpr auto kHotBranch = program(kPollHotBranchAddress, kPollHotBranchWords);
constexpr auto kAbsolute = program(kPollAbsoluteAddress, kPollAbsoluteWords, 0x1000u, 0u);
constexpr auto kCompareOne = program(kPollCompareOneAddress, kPollCompareOneWords, kFlag, 3u, 0u, 9u);

// The loop's back edge (its one backward bc) and the head it targets, read
// from the fixture's own words.
u32 back_edge_site(const Program& p) {
    for (std::size_t i = 0; i < p.count; ++i) {
        const u32 word = p.words[i];
        if ((word >> 26) == 16u && (word & 0x8000u) != 0u) return p.address + static_cast<u32>(i * 4u);
    }
    throw std::runtime_error("fixture program has no back edge");
}

u32 loop_head(const Program& p) {
    const u32 site = back_edge_site(p);
    const u32 word = p.words[(site - p.address) / 4u];
    return site + static_cast<u32>(static_cast<s32>(static_cast<s16>(word & 0xFFFCu)));
}

enum class Route { Direct, Range };
// What ends a loop that only an event can end.
enum class Release {
    None,       // nothing: the loop's own reads end it
    Interrupt,  // a decrementer edge delivered (and deliverable) before entry; its handler stores the flag
    HostStop,   // a host stop (the wait-point watchdog) posted; the host writes the flag
};
struct Case {
    const char* name;
    const Program& program;
    Route route;
    u32 offset;
    u32 initial_register;
    u32 initial_flag;
    Release release;
    u64 to_back_edge;  // interpreted instructions up to the taken back edge that hands back (0: none)
    u64 instructions;  // interpreted instructions the reference executes up to the return
};

// Wait cases: to_back_edge is the iteration (or partial iteration) before
// the hand-back; then the handler (Interrupt) and one full exiting iteration.
constexpr Case kCases[] = {
    {"alias/direct", kAlias, Route::Direct, 0u, kFlag, 0u, Release::None, 0u, 7u},
    {"alias/range-load", kAlias, Route::Range, 0u, kFlag, 0u, Release::None, 0u, 7u},
    {"alias/range-compare", kAlias, Route::Range, 4u, 0u, 0u, Release::None, 0u, 6u},
    {"alias/range-branch", kAlias, Route::Range, 8u, 0u, 0u, Release::None, 0u, 5u},
    {"alias/ready", kAlias, Route::Direct, 0u, kFlag, 1u, Release::None, 0u, 4u},
    {"stable/ready", kStable, Route::Direct, 0u, 0u, 1u, Release::None, 0u, 4u},
    {"stable/range-load-ready", kStable, Route::Range, 0u, 0u, 1u, Release::None, 0u, 4u},
    {"stable/range-compare-stale", kStable, Route::Range, 4u, 0u, 1u, Release::None, 0u, 6u},
    {"stable/range-branch-stale", kStable, Route::Range, 8u, 0u, 1u, Release::None, 0u, 5u},
    {"stable/range-compare-ready", kStable, Route::Range, 4u, 1u, 1u, Release::None, 0u, 3u},
    {"stable/ready-event-pending", kStable, Route::Direct, 0u, 0u, 1u, Release::Interrupt, 0u, 4u},
    {"stable/wait-interrupt", kStable, Route::Direct, 0u, 0u, 0u, Release::Interrupt, 3u, 7u + kHandlerInstructions},
    {"stable/wait-host-stop", kStable, Route::Direct, 0u, 0u, 0u, Release::HostStop, 3u, 7u},
    {"stable/range-load-wait-interrupt", kStable, Route::Range, 0u, 0u, 0u, Release::Interrupt, 3u,
     7u + kHandlerInstructions},
    {"stable/range-load-wait-host-stop", kStable, Route::Range, 0u, 0u, 0u, Release::HostStop, 3u, 7u},
    {"stable/range-compare-wait-interrupt", kStable, Route::Range, 4u, 0u, 0u, Release::Interrupt, 2u,
     6u + kHandlerInstructions},
    {"stable/range-compare-wait-host-stop", kStable, Route::Range, 4u, 0u, 0u, Release::HostStop, 2u, 6u},
    {"stable/range-branch-wait-interrupt", kStable, Route::Range, 8u, 0u, 0u, Release::Interrupt, 1u,
     5u + kHandlerInstructions},
    {"stable/range-branch-wait-host-stop", kStable, Route::Range, 8u, 0u, 0u, Release::HostStop, 1u, 5u},
    {"hot-compare/direct", kHotCompare, Route::Direct, 0u, 0u, 1u, Release::None, 0u, 7u},
    {"hot-compare/range-root", kHotCompare, Route::Range, 0u, 0u, 1u, Release::None, 0u, 7u},
    {"hot-compare/range-load", kHotCompare, Route::Range, 4u, 0u, 1u, Release::None, 0u, 4u},
    {"hot-compare/range-compare", kHotCompare, Route::Range, 8u, 0u, 1u, Release::None, 0u, 6u},
    {"hot-compare/range-branch", kHotCompare, Route::Range, 12u, 0u, 1u, Release::None, 0u, 5u},
    {"hot-compare/wait-interrupt", kHotCompare, Route::Direct, 0u, 0u, 0u, Release::Interrupt, 3u,
     7u + kHandlerInstructions},
    {"hot-compare/wait-host-stop", kHotCompare, Route::Direct, 0u, 0u, 0u, Release::HostStop, 3u, 7u},
    {"hot-branch/direct", kHotBranch, Route::Direct, 0u, 0u, 1u, Release::None, 0u, 6u},
    {"hot-branch/range-root", kHotBranch, Route::Range, 0u, 0u, 1u, Release::None, 0u, 6u},
    {"hot-branch/range-load", kHotBranch, Route::Range, 4u, 0u, 1u, Release::None, 0u, 4u},
    {"hot-branch/range-compare", kHotBranch, Route::Range, 8u, 0u, 1u, Release::None, 0u, 6u},
    {"hot-branch/range-branch", kHotBranch, Route::Range, 12u, 0u, 1u, Release::None, 0u, 5u},
    {"hot-branch/wait-interrupt", kHotBranch, Route::Direct, 0u, 0u, 0u, Release::Interrupt, 2u,
     6u + kHandlerInstructions},
    {"hot-branch/wait-host-stop", kHotBranch, Route::Direct, 0u, 0u, 0u, Release::HostStop, 2u, 6u},
    {"absolute/wait-interrupt", kAbsolute, Route::Direct, 0u, 0xABCDEF01u, 0u, Release::Interrupt, 3u,
     7u + kHandlerInstructions},
    {"absolute/wait-host-stop", kAbsolute, Route::Direct, 0u, 0xABCDEF01u, 0u, Release::HostStop, 3u, 7u},
    {"absolute/range-load-wait-interrupt", kAbsolute, Route::Range, 0u, 0xABCDEF01u, 0u, Release::Interrupt,
     3u, 7u + kHandlerInstructions},
    {"absolute/range-load-wait-host-stop", kAbsolute, Route::Range, 0u, 0xABCDEF01u, 0u, Release::HostStop,
     3u, 7u},
    {"absolute/ready", kAbsolute, Route::Direct, 0u, 0xABCDEF01u, 1u, Release::None, 0u, 4u},
    {"absolute/range-compare", kAbsolute, Route::Range, 4u, 0u, 1u, Release::None, 0u, 6u},
    {"absolute/range-branch", kAbsolute, Route::Range, 8u, 0u, 1u, Release::None, 0u, 5u},
    {"compare-one/wait-interrupt", kCompareOne, Route::Direct, 0u, 0u, 1u, Release::Interrupt, 3u,
     7u + kHandlerInstructions},
    {"compare-one/wait-host-stop", kCompareOne, Route::Direct, 0u, 0u, 1u, Release::HostStop, 3u, 7u},
    {"compare-one/ready", kCompareOne, Route::Direct, 0u, 1u, 0u, Release::None, 0u, 4u},
    {"compare-one/range-compare", kCompareOne, Route::Range, 4u, 1u, 0u, Release::None, 0u, 6u},
    {"compare-one/range-branch", kCompareOne, Route::Range, 8u, 1u, 0u, Release::None, 0u, 5u},
};

bool hands_back(const Case& test) { return test.to_back_edge != 0u; }
bool handler_runs(const Case& test) { return hands_back(test) && test.release == Release::Interrupt; }

// The game clock, every deadline on it and its time-base and decrementer views.
struct Timeline {
    u64 now = 0;
    std::array<u64, static_cast<std::size_t>(GameEvent::Count)> deadlines{};
    u64 time_base = 0;
    u32 decrementer = 0;
    bool operator==(const Timeline& other) const {
        return now == other.now && deadlines == other.deadlines && time_base == other.time_base &&
               decrementer == other.decrementer;
    }
};
Timeline timeline(const Cpu& c) {
    return {c.game_clock().now(), c.game_clock().deadlines(), c.time_base(), c.decrementer()};
}

// The fixture's data page at 0: the shapes read EA 0 (an aliased base that
// reached zero), 0x1000 (an absolute rA = 0 address) and the flag / marker
// words. Only a page mapped there can serve them; the game maps nothing there.
constexpr u32 kDataSize = 0x20000u;

GuestMemory& with_data_page(GuestMemory& memory) {
    if (!memory.map(0u, kDataSize, kDataSize, GuestMemory::RegionKind::Arena, "poll-fixture-data"))
        throw std::runtime_error("cannot map the fixture's data page");
    return memory;
}

struct Fixture {
    GuestMemory bus;
    Cpu cpu{with_data_page(bus)};
    u64 idle_waits = 0;

    explicit Fixture(const Case& test) {
        const Program& p = test.program;
        for (u32 i = 0; i < 32u; ++i) {
            cpu.gpr[i] = 0xA5000000u + i * 0x010101u;
            cpu.fpr[i].bits[0] = 0x0123456789ABCDEFull ^ i;
            cpu.fpr[i].bits[1] = 0xFEDCBA9876543210ull ^ (u64(i) << 32u);
        }
        for (u32 i = 0; i < 16u; ++i) cpu.sr[i] = 0x10000000u + i * 0x101u;
        for (u32 i = 0; i < 4u; ++i) cpu.spr[SPR_SPRG0 + i] = 0xBA000000u | i;
        cpu.cr = 0x30000000u | kCrTail;
        cpu.xer = kXerSO | kXerOV | kXerCA | 0x55u;
        cpu.ctr = 0xFEDCBA98u;
        cpu.fpscr = 0x9ABCDEF0u;
        cpu.lr = kReturn;
        cpu.set_decrementer(kDecrementer);
        cpu.gpr[p.result_register] = test.initial_register;
        cpu.gpr[4] = kFlag;
        cpu.gpr[28] = kMarker;      // the handler's operands (kHandlerWords)
        cpu.gpr[29] = p.exit_value;
        cpu.gpr[30] = p.flag;
        cpu.set_msr(cpu.msr() | kMsrEE);  // the guest polls with interrupts enabled
        cpu.set_entry(p.address + test.offset);
        for (std::size_t i = 0; i < p.count; ++i)
            bus.write32((p.address) + static_cast<u32>(i * 4u), p.words[i]);
        // The decrementer's OS entry as __OSExceptionInit and OSSetCurrentContext
        // leave it: the vector page with `li r3, 8` at +0x68, the handler in
        // __OSExceptionTable (0x80003000), the current OSContext at 0xD4.
        write_os_exception_vector(bus, 0x80000000u + static_cast<u32>(Exception::Decrementer), 8u);
        bus.write32(0x80003000u + 4u * 8u, kHandler);
        bus.write32(0x800000D4u, kContext);
        for (std::size_t i = 0; i < kHandlerInstructions; ++i)
            bus.write32(kHandler + static_cast<u32>(i * 4u), kHandlerWords[i]);
        bus.write32(0u, 1u);
        bus.write32(p.flag, test.initial_flag);
        bus.write32(kMarker, 0u);
        bus.write8(GuestMemory::kLegacyHeapBase + 3u, 0x5Au);
        bus.write8(kLockedCacheBase + 7u, 0xC3u);
        u32 translated = 0;
        for (u32 address : {0u, kFlag, kMarker, 0x1FFFCu})
            require(cpu.translate(address, false, false, translated) && translated == address &&
                        bus.mapped(GcAddr(address), 4u),
                    "the fixture's data page must serve its own addresses");
        require(cpu.translate(cpu.pc, false, true, translated) && translated == cpu.pc,
                "an instruction address must translate to itself");
        require(cpu.game_clock().now() == 0u && cpu.time_base() == 0u &&
                    cpu.interpreted_instructions() == 0u,
                "fixture must start at game time zero with nothing interpreted");
        // No wait point is listed for this image. A counting clock owner makes
        // any emitted wait visible instead of stopping the machine.
        cpu.set_idle_handler([this](Cpu&) { ++idle_waits; });
        if (test.release == Release::Interrupt) {
            // The scheduler delivered the decrementer edge at the wait point
            // before the guest ran (System::advance_game_time): the clock at
            // the deadline, DEC = -1, the condition latched and, with EE = 1,
            // deliverable.
            GameClock& clock = cpu.game_clock();
            clock.set_now(clock.deadline(GameEvent::Decrementer));
            cpu.latch_decrementer();
            require(cpu.interrupt_pending() && cpu.decrementer() == 0xFFFFFFFFu,
                    "the delivered decrementer edge must be deliverable");
        }
    }
};

void golden(const Fixture& fixture, const Case& test, const Timeline& entry) {
    const Cpu& c = fixture.cpu;
    const Program& p = test.program;
    require(!c.stopped() && c.pc == kReturn, "did not return to LR: " + c.stop().detail);
    require(timeline(c) == entry, "guest code moved the game clock or a deadline");
    require(c.gpr[p.result_register] == p.exit_value &&
                c.cr == ((p.exit_cr0 << 28u) | kCrTail), "result/CR golden mismatch");
    require(fixture.bus.peek32(kMarker) == (handler_runs(test) ? 1u : 0u),
            "decrementer handler ledger golden mismatch");
    require(fixture.bus.peek32(p.flag) == (hands_back(test) ? p.exit_value : test.initial_flag),
            "polled flag golden mismatch");
    require(c.decrementer_pending() == (test.release == Release::Interrupt && !hands_back(test)),
            "decrementer condition golden mismatch");
    if (handler_runs(test)) {
        require(c.spr[SPR_SRR0] == loop_head(p) && (c.spr[SPR_SRR1] & kMsrEE) != 0u,
                "the decrementer must be taken at the loop head with MSR[EE] = 1");
    }
    require(fixture.idle_waits == 0u && !fixture.bus.watch_active(),
            "an unlisted loop must never wait or start a poll watch");
    require(fixture.bus.diagnostics().unmapped_reads == 0u &&
                fixture.bus.diagnostics().unmapped_writes == 0u, "unmapped bus access");
}

void compare(Fixture& native, Fixture& oracle) {
    const Cpu& n = native.cpu;
    const Cpu& o = oracle.cpu;
    require(std::memcmp(n.gpr, o.gpr, sizeof(n.gpr)) == 0, "GPR mismatch");
    require(std::memcmp(n.fpr, o.fpr, sizeof(n.fpr)) == 0, "FPR mismatch");
    require(n.spr == o.spr && n.decrementer() == o.decrementer() && n.sr == o.sr,
            "SPR/segment mismatch");
    require(n.pc == o.pc && n.lr == o.lr && n.ctr == o.ctr && n.cr == o.cr &&
                n.xer == o.xer && n.msr() == o.msr() && n.fpscr == o.fpscr &&
                n.reservation == o.reservation,
            "control state mismatch");
    require(timeline(n) == timeline(o) && n.decrementer_pending() == o.decrementer_pending() &&
                n.external_interrupt() == o.external_interrupt(),
            "game clock/interrupt mismatch");
    require(window_snapshot(native.bus) == window_snapshot(oracle.bus), "RAM mismatch");
}

// After the return both machines' time-base and decrementer views are the same
// function of the game clock. (The instruction clock's sub-tick residue this
// used to compare has no successor: nothing ticks between wait points.)
void clock_views(Fixture& native, Fixture& oracle) {
    const u64 start = native.cpu.game_clock().now();
    const u64 tb = native.cpu.time_base();
    const u32 dec = native.cpu.decrementer();
    for (u64 ticks = 1u; ticks <= 12u; ++ticks) {
        native.cpu.game_clock().set_now(start + ticks);
        oracle.cpu.game_clock().set_now(start + ticks);
        require(native.cpu.time_base() == tb + ticks && oracle.cpu.time_base() == tb + ticks &&
                    native.cpu.decrementer() == dec - static_cast<u32>(ticks) &&
                    native.cpu.decrementer() == oracle.cpu.decrementer(),
                "time-base/decrementer views differ after return");
    }
}

void run_case(RecompiledCode& code, const Case& test) {
    auto native = std::make_unique<Fixture>(test);
    auto oracle = std::make_unique<Fixture>(test);
    native->cpu.attach_recompiled(&code);
    native->cpu.set_record_fallbacks(true);
    oracle->cpu.attach_recompiled(nullptr);
    const Program& p = test.program;
    const u32 entry = p.address + test.offset;
    const u32 head = loop_head(p);
    const u32 site = back_edge_site(p);
    const auto fn = code.lookup(p.address);
    const auto* range = code.find_range(entry);
    require(fn != nullptr && range != nullptr && range->at != nullptr &&
                range->begin == p.address, "missing generated fixture function/range");
    require(code.lookup(head) != nullptr, "the loop head has no generated entry to resume at");
    const Timeline entry_time = timeline(native->cpu);
    require(timeline(oracle->cpu) == entry_time, "the two fixtures start at different game times");
    CpuDiagCounters& diag = cpu_diag();
    const u64 before_case = g_non_native_ppc_executions.load();

    // The generated function, entered the way Cpu::run enters it. A host stop
    // is posted first: a watchdog thread posting while the loop spins raises
    // the same flag the same back-edge poll reads, and posting before the call
    // keeps the case deterministic (gekko_core_tests posts from a thread).
    const u64 polls_before = diag.poll_points;
    if (test.release == Release::HostStop)
        native->cpu.request_stop_from_host(StopReason::HostRequest, "generated poll test host stop");
    if (test.route == Route::Direct) {
        require(test.offset == 0u, "direct route must use the function root");
        fn(native->cpu);
    } else {
        range->at(native->cpu, entry);
    }
    const u64 native_non_native = g_non_native_ppc_executions.load() - before_case;
    const u64 hand_backs = native->cpu.interpreter_exits;
    const u32 hand_back_pc = native->cpu.pc;
    const bool native_waited = native->idle_waits != 0u || diag.poll_points != polls_before ||
                               native->bus.watch_active();
    u64 oracle_steps = 0;
    u64 dispatches = 0;
    std::string stop_detail;
    if (hands_back(test) && hand_backs == 1u && hand_back_pc == head) {
        // The reference interpreter up to the same instruction boundary: the
        // two machines must agree there exactly.
        while (oracle_steps < test.to_back_edge && !oracle->cpu.stopped()) {
            oracle->cpu.step();
            ++oracle_steps;
        }
        try {
            compare(*native, *oracle);
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string(test.name) + ": at the hand-back: " + error.what());
        }
        if (test.release == Release::Interrupt) {
            // The dispatcher takes the decrementer at R1 (the native OS entry),
            // interprets the handler (no native code there in this image) one
            // instruction per dispatch, and resumes the generated loop at its head.
            dispatches = native->cpu.run_steps(kHandlerInstructions + 1u);
        } else {
            // The dispatcher serves the host stop at once, naming the head.
            dispatches = native->cpu.run_steps(1u);
            stop_detail = native->cpu.stop().detail;
            require(native->cpu.stopped() && native->cpu.stop().reason == StopReason::HostRequest &&
                        native->cpu.pc == head,
                    std::string(test.name) + ": the posted host stop was not served at the loop head: " +
                        stop_detail);
            // The event at the wait point after the stop releases the flag,
            // in both machines; then the machine resumes.
            native->bus.write32(p.flag, p.exit_value);
            oracle->bus.write32(p.flag, p.exit_value);
            native->cpu.clear_stop();
            dispatches += native->cpu.run_steps(1u);
        }
    }
    while (!oracle->cpu.stopped() && oracle->cpu.pc != oracle->cpu.lr && oracle_steps < kStepCap) {
        oracle->cpu.step();
        ++oracle_steps;
    }
    const u64 case_non_native = g_non_native_ppc_executions.load() - before_case;
    const u64 native_interpreted = native->cpu.interpreted_instructions();
    const u64 oracle_interpreted = oracle->cpu.interpreted_instructions();
    std::ostringstream detail;
    detail << "native non-native=" << native_non_native << " hand-backs=" << hand_backs
           << " at=" << hex(hand_back_pc) << " dispatches=" << dispatches
           << " interpreted=" << native_interpreted << " marker=" << native->bus.peek32(kMarker)
           << "; oracle steps=" << oracle_steps << " interpreted=" << oracle_interpreted
           << " marker=" << oracle->bus.peek32(kMarker)
           << "; expected oracle instructions=" << test.instructions
           << " hand-back=" << (hands_back(test) ? hex(head) : std::string("none"))
           << " after " << test.to_back_edge;
    try {
        require(native_non_native == 0u && !native_waited,
                "the generated loop must run natively and never wait (no wait point is listed here)");
        require(hand_backs == (hands_back(test) ? 1u : 0u),
                "the generated loop handed back where it must not, or did not where it must");
        require(native->cpu.fallback_total(FallbackReason::Yield) == hand_backs,
                "a hand-back that is not a Yield");
        if (hands_back(test)) {
            const auto sites = native->cpu.fallback_sites();
            require(hand_back_pc == head && sites.size() == 1u && sites[0].target == head &&
                        sites[0].caller == site && sites[0].reason == FallbackReason::Yield &&
                        sites[0].count == 1u,
                    "the first taken back edge must hand back at the loop head from the back-edge site");
            require(dispatches == (test.release == Release::Interrupt ? kHandlerInstructions + 1u : 2u),
                    "the dispatcher did not resume the generated loop as expected");
            if (test.release == Release::HostStop) {
                require(stop_detail.find("guest pc=" + hex(head)) != std::string::npos,
                        "the host stop must name the loop head it was served at");
            }
        } else {
            require(native->cpu.fallback_sites().empty(), "a loop that returns must not hand back");
        }
        require(native->cpu.exiting() == (test.release == Release::Interrupt && !hands_back(test)),
                "the generated code must return normally; only a still-deliverable interrupt may "
                "request the caller's unwind");
        require(oracle->cpu.pc == oracle->cpu.lr && !oracle->cpu.stopped(),
                "reference oracle failed to return within 1000 steps: " + oracle->cpu.stop().detail);
        require(oracle_steps == test.instructions && oracle_interpreted == oracle_steps,
                "reference instruction golden mismatch");
        require(native_interpreted == (handler_runs(test) ? kHandlerInstructions : 0u),
                "the native machine interpreted more than the handler");
        require(case_non_native == oracle_interpreted + native_interpreted,
                "the non-native execution ledger does not add up");
        golden(*oracle, test, entry_time);
        golden(*native, test, entry_time);
        compare(*native, *oracle);
        clock_views(*native, *oracle);
    } catch (const std::exception& error) {
        throw std::runtime_error(detail.str() + ": " + error.what());
    }
    std::cout << "PASS " << test.name << ": " << detail.str() << '\n';
}
}

int main() {
    try {
        require(reference_execution_available(), "reference provider is required for the test oracle");
        RecompiledCode code;
        code.install(recomp::kFunctions, recomp::kFunctionCount);
        code.install_ranges(recomp::kRanges, recomp::kRangeCount);
        require(code.lookup(kReturn) == nullptr && code.find_range(kReturn) == nullptr,
                "return sentinel must remain outside generated code");
        require(code.lookup(kHandler) == nullptr && code.find_range(kHandler) == nullptr,
                "the decrementer handler must have no native code in this image");
        unsigned failures = 0;
        for (const Case& test : kCases) {
            try { run_case(code, test); }
            catch (const std::exception& error) {
                std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
                ++failures;
            }
        }
        std::cout << sizeof(kCases) / sizeof(kCases[0]) << " generated poll cases, "
                  << failures << " failures\n";
        return failures == 0u ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
