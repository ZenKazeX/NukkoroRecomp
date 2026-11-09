// Verify the actual generated game bodies, with no compiler-runtime HLE hooks.
#include "gekko/system.h"
#include "gekko/recomp.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gekko;
namespace {
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
std::string hex(u32 value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}
DolProgram load(const char* path) {
    DolProgram dol;
    std::ifstream stream(path, std::ios::binary);
    dol.bytes.assign(std::istreambuf_iterator<char>(stream), {});
    require(dol.bytes.size() >= 256u, "cannot read fixture DOL");
    const auto word = [&](u32 offset) {
        const auto* b = dol.bytes.data() + offset;
        return (u32(b[0]) << 24) | (u32(b[1]) << 16) | (u32(b[2]) << 8) | b[3];
    };
    for (u32 i = 0; i < 18; ++i) {
        DolSectionInfo section{word(i * 4u), word(0x48u + i * 4u), word(0x90u + i * 4u), i < 7};
        require(u64(section.file_offset) + section.size <= dol.bytes.size(), "bad DOL section");
        if (section.size != 0) dol.sections.push_back(section);
    }
    dol.entry_point = word(0xE0u); dol.bss_address = word(0xD8u); dol.bss_size = word(0xDCu);
    return dol;
}

// Guest code runs in zero game time (docs/native-time-20261005/DESIGN.md): the
// clock, every deadline on it and the time-base and decrementer views it
// carries are exactly as a call found them, whichever engine ran it.
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

// The instruction clock's twelve time-base phases (where inside a tick a
// helper started) are gone with it. What remains to sweep is the distance to
// the next timed event: the decrementer's 0 -> 1 edge armed `distance` + 1
// ticks out (distance 0: the very next tick), with MSR[EE] = 1 so that an edge
// delivered inside a helper would be taken. No helper may reach it.
void arm_next_event(Cpu& c, u32 distance) {
    c.set_msr(c.msr() | kMsrEE);
    c.set_decrementer(distance);
}

// The 64-bit compiler helpers - the tick wrapper 0x80051BBC, the TB->us
// scaling 0x80051414 and the unsigned and signed divides - each verified
// call by call against the interpreter (--verify-recomp).
void verify_compiler_helper(System& system, RecompiledCode& code, u32 entry) {
    Cpu& c = system.cpu();
    constexpr u64 values[][2] = {{0,1}, {1,2}, {0xFFFFFFFFFFFFFFFFull,10},
        {0x8000000000000000ull,0xFFFFFFFFFFFFFFFFull},
        {0xFEDCBA9876543210ull,0x123456789ull}, {0x100000000ull,0xFFFFFFFFull}};
    u64 segments = 0, instructions = 0;
    unsigned cases = 0;
    for (const auto& v : values) for (u64 tb : {0x123456789ABCull, 0xFFFFFFFEull, 0xFFFFFFFFull}) {
        u64 stream = 0;  // replayed instructions, the same at every event distance
        for (u32 distance = 0; distance < 12; ++distance) {
            c.reset(); c.set_verify_recompiled(false); c.attach_recompiled(&code);
            c.set_entry(entry); c.lr = 0x800F1000u; c.gpr[1] = 0x800F0000u;
            c.gpr[2] = 0x805FA780u; c.gpr[13] = 0x805EC500u;
            c.gpr[3] = u32(v[0] >> 32); c.gpr[4] = u32(v[0]);
            c.gpr[5] = u32(v[1] >> 32); c.gpr[6] = u32(v[1]);
            c.cr = 0x13579BDFu; c.xer = kXerCA | kXerSO | kXerOV;
            c.mem_write32(0x800000F8u, 162000000u);
            arm_next_event(c, distance); c.set_time_base(tb);
            const Timeline entry_time = timeline(c);
            const u64 waits_before = system.idle_waits();
            require(!c.try_hle(entry), "guest compiler body was intercepted");
            const u64 verified_before = c.verify_report().verified;
            const u64 replayed_before = c.verify_report().verified_instructions;
            c.set_verify_recompiled(true);
            unsigned dispatches = 0;
            while (!c.stopped() && c.pc != 0x800F1000u && dispatches++ < 10000u) c.run_steps(1u);
            const auto& report = c.verify_report();
            require(!c.stopped() && c.pc == 0x800F1000u, c.stop().detail);
            require(report.verified > verified_before && report.mismatches == 0 &&
                        report.skipped_device == 0 && report.skipped_exit == 0,
                    "helper was mismatched or not fully verified");
            // The verified segments partition the call (each replay ends
            // where the next dispatch starts), so their replays are the
            // helper's whole instruction stream; with no time passing inside
            // it, the distance to the next event cannot change that stream.
            const u64 replayed = report.verified_instructions - replayed_before;
            require(replayed > 0u && (distance == 0u || replayed == stream),
                    "the verified instruction stream depends on the distance to the next event");
            stream = replayed;
            // 0x80051414 reads the time base through OSGetTick, whose
            // mftbu/mftb/mftbu retry is a listed `clock` wait point
            // (0x80379638): the time base cannot change between its two
            // mftbu inside guest code, so that back edge - and its
            // idle_wait - is never taken.
            require(timeline(c) == entry_time && !c.decrementer_pending() &&
                        system.idle_waits() == waits_before,
                    "a helper moved the game clock, delivered an event or waited");
            segments += report.verified - verified_before;
            instructions += replayed;
            ++cases;
        }
    }
    std::cout << "PASS 0x" << std::hex << entry << std::dec << ": " << cases
              << " cases, " << segments << " verified segments, " << instructions
              << " verified instructions; next event 1..12 ticks out and TBL rollover, "
                 "game clock untouched\n";
}

// Audio decoding: execute the actual emitted bodies and their interior
// resumes against the PPC interpreter, including saturation, predictor
// history, CTR loops, CR/XER, stack saves and every distance to the next
// timed event.
void verify_audio_decoder(System& system, RecompiledCode& code, u32 entry) {
    Cpu& c = system.cpu();
    u64 segments = 0, instructions = 0, native_interpreted = 0;
    u64 oracle_carry_corrections = 0;
    unsigned cases = 0;
    for (u32 pattern = 0; pattern < 8; ++pattern) for (u32 distance = 0; distance < 12; ++distance) {
        c.set_verify_recompiled(false); c.reset(); c.attach_recompiled(&code);
        c.set_entry(entry); c.lr = 0x800F1000u; c.gpr[1] = 0x800F0000u;
        c.gpr[2] = 0x805FA780u; c.gpr[13] = 0x805EC500u;
        c.gpr[3] = 0x80070000u; c.gpr[4] = pattern == 0 ? 0u : 4u;
        c.gpr[5] = 0x80080000u; c.gpr[6] = 0x800A0000u;
        c.gpr[7] = 0x80090000u; c.gpr[8] = 0x800A0010u;
        c.gpr[9] = 0x1F00u; c.gpr[10] = 0xF100u;
        c.mem_write32(c.gpr[1] + 8u, 0x800A0020u);
        c.mem_write16(c.gpr[1] + 14u, 0x100u);
        c.mem_write16(c.gpr[1] + 18u, 1u);
        if (entry == 0x803C607Cu) {
            c.gpr[7] = 0x1F00u; c.gpr[8] = 0xF100u;
            c.gpr[9] = 0x800A0020u; c.gpr[10] = 0x100u;
            c.mem_write16(c.gpr[1] + 10u, 1u);
        }
        for (u32 i = 0; i < 144u; ++i)
            c.mem_write8(0x80070000u + i, static_cast<u8>(pattern * 31u + i * 53u));
        for (u32 i = 0; i < 8u; ++i)
            c.mem_write16(0x80070000u + i * 18u,
                pattern == 7 ? 0x8000u : static_cast<u16>(pattern * 127u));
        for (u32 i = 0; i < 36u; i += 2u)
            c.mem_write16(0x800A0000u + i, static_cast<u16>(pattern * 8191u));
        c.mem_write16(0x800A0020u, 0u);
        c.cr = 0x13579BDFu; c.xer = kXerCA | kXerSO | kXerOV; c.ctr = 0xACEDu;
        arm_next_event(c, distance); c.set_time_base(0xFFFFFFFFull);
        const Timeline entry_time = timeline(c);
        const u64 waits_before = system.idle_waits();
        std::vector<u8> initial_state;
        c.save_state(initial_state);
        // The game's memory: the program image and the OS heap region after it.
        const auto memory_now = [&] {
            return system.guest_memory().window_bytes(GuestMemory::kImageBase, GuestMemory::kLegacyHeapEnd);
        };
        const auto initial_memory = memory_now();
        unsigned dispatches = 0;
        while (!c.stopped() && c.pc != 0x800F1000u && dispatches++ < 10000u) c.run_steps(1u);
        require(!c.stopped() && c.pc == 0x800F1000u, c.stop().detail);
        const std::vector<u32> native_gpr(std::begin(c.gpr), std::end(c.gpr));
        const auto native_spr = c.spr;
        const u32 native_dec = c.decrementer();
        const auto native_sr = c.sr;
        const u32 native_cr = c.cr, native_xer = c.xer, native_ctr = c.ctr,
                  native_lr = c.lr, native_msr = c.msr();
        const u64 native_tb = c.time_base();
        const Timeline native_time = timeline(c);
        const bool native_pending = c.decrementer_pending();
        native_interpreted += c.interpreted_instructions();
        const auto native_memory = memory_now();
        system.guest_memory().write_block(GuestMemory::kImageBase, initial_memory.data(), initial_memory.size());
        const u8* cursor = initial_state.data();
        // Savestate version 11: the game clock is part of the CPU state.
        require(c.load_state(cursor, cursor + initial_state.size(), 11u), "restore audio oracle");
        c.attach_recompiled(nullptr);
        const u64 oracle_before = c.interpreted_instructions();
        unsigned steps = 0;
        while (!c.stopped() && c.pc != 0x800F1000u && steps++ < 100000u) {
            const u32 word = c.mem_read32(c.pc);
            const u32 source = c.gpr[(word >> 21u) & 31u];
            const u32 shift = (word >> 11u) & 31u;
            c.step();
            // Independent carry oracle, IBM Gekko manual p.540:
            // srawi CA uses the ORIGINAL source's shifted-out bits.
            // Cpu::op_group31 used to read gpr[d] after storing gpr[a]
            // and this test patched XER to hide it. The interpreter now
            // latches rS first (audit finding sqrt-ca-lead-1), so any
            // disagreement with the independent oracle is a failure.
            if ((word >> 26u) == 31u && ((word >> 1u) & 1023u) == 824u) {
                const bool carry = (source & 0x80000000u) != 0u &&
                    (source & ((1u << shift) - 1u)) != 0u;
                const u32 expected = (c.xer & ~kXerCA) | (carry ? kXerCA : 0u);
                oracle_carry_corrections += expected != c.xer;
                require(expected == c.xer,
                        "reference interpreter srawi XER[CA] disagrees with the "
                        "independent original-rS carry oracle");
            }
        }
        require(!c.stopped() && c.pc == 0x800F1000u, "audio oracle failed to return");
        // The retired-instruction count the two engines used to share is
        // gone; what it carried - the game time both left behind - is the
        // clock, which neither may have moved.
        require(std::equal(native_gpr.begin(), native_gpr.end(), std::begin(c.gpr)) &&
                    native_spr == c.spr && native_dec == c.decrementer() &&
                    native_sr == c.sr && native_cr == c.cr &&
                    native_xer == c.xer && native_ctr == c.ctr && native_lr == c.lr &&
                    native_msr == c.msr() && native_tb == c.time_base() &&
                    native_time == timeline(c) && native_pending == c.decrementer_pending() &&
                    native_memory == memory_now(),
                "audio codegen differs from original PPC and independent srawi carry oracle");
        require(native_time == entry_time && !native_pending && system.idle_waits() == waits_before,
                "audio codegen moved the game clock, delivered an event or waited");
        segments += dispatches;
        instructions += c.interpreted_instructions() - oracle_before;
        ++cases;
    }
    std::cout << "PASS audio 0x" << std::hex << entry << std::dec << ": " << cases
              << " cases, " << segments << " segments, " << instructions
              << " original PPC instructions matched (" << native_interpreted
              << " interpreted on the native side); " << oracle_carry_corrections
              << " reference-interpreter CA disagreements with the independent oracle\n";
}

// The SelectThread idle loop, the RAM-poll shape the deleted coalesce_ram_poll
// matched:  0x803788FC  lwz r0,0x5AE0(r13) ; cmplwi r0,0 ; beq 0x803788FC.
// It is a listed wait point (extract/wait-points.txt `0x80378904 0x803788FC
// poll`, tools/ppcrecomp/wait_points.py), so the generator emits on its taken
// back edge (emit_wait_point):
//   c.pc = 0x803788FCu; c.idle_wait();
//   if (c.needs_dispatch()) { c.leave_to_dispatcher(0x803788FCu, FallbackReason::Yield, 0x80378904u); return; }
constexpr u32 kIdleLoop = 0x803788FCu;
constexpr u32 kIdleBackEdge = 0x80378904u;
constexpr u32 kSda = 0x805EC500u;
constexpr u32 kRunQueueBits = kSda + 0x5AE0u;  // r13-relative, the word the loop polls
constexpr u32 kIdleEdge = 500u;                 // decrementer value: the edge 501 ticks out

bool single_fallback(const Cpu& c, u32 target, u32 caller, FallbackReason reason) {
    const auto sites = c.fallback_sites();
    return sites.size() == 1u && sites[0].target == target && sites[0].caller == caller &&
           sites[0].reason == reason && sites[0].count == 1u;
}

void wait_point_contract(System& system, RecompiledCode& code) {
    Cpu& c = system.cpu();
    require(code.lookup(kIdleLoop) != nullptr, "the SelectThread idle loop head has no generated entry");
    require(!c.has_hle(kIdleLoop), "a host hook serves the SelectThread idle loop");
    const auto prepare = [&](bool verify, u32 run_queue_bits) {
        c.set_verify_recompiled(false); c.reset(); c.attach_recompiled(&code);
        c.set_record_fallbacks(true);
        c.set_entry(kIdleLoop); c.lr = 0x800F1000u; c.gpr[1] = 0x800F0000u;
        c.gpr[2] = 0x805FA780u; c.gpr[13] = kSda;
        // SelectThread polls with interrupts enabled (OSEnableInterrupts at
        // 0x803788F8, the call before the loop).
        c.set_msr(c.msr() | kMsrEE);
        c.mem_write32(kRunQueueBits, run_queue_bits);
        c.set_verify_recompiled(verify);
    };

    // No thread is ready. Every taken back edge waits (System::idle_wait: the
    // clock advances to its next event and delivers it), the loop re-reads
    // RunQueueBits after every wait, and the wait that delivers the
    // decrementer edge makes it deliverable: the back edge's needs_dispatch()
    // hands back at the loop head from the listed site, with the clock exactly
    // at that edge. Under --verify-recomp the segment delivered events the
    // oracle cannot replay, so it is skipped as device access and the native
    // result stands, identical to the plain run.
    for (bool verify : {false, true}) {
        const std::string mode = verify ? " (verified)" : "";
        prepare(verify, 0u);
        const u64 armed = system.game_ticks();
        c.set_decrementer(kIdleEdge);
        u64 reads = 0;
        system.guest_memory().set_access_trace([&reads](u32 address, std::size_t size, bool writing) {
            if (!writing && size == 4u && address == kRunQueueBits) ++reads;
        });
        const u64 waits_before = system.idle_waits();
        const u64 steps = c.run_steps(1u);
        system.guest_memory().set_access_trace({});
        const u64 waits = system.idle_waits() - waits_before;
        require(steps == 1u && !c.stopped() && c.pc == kIdleLoop,
                "the waiting idle loop must hand back at its head" + mode + ": " + c.stop().detail);
        require(waits >= 1u && reads == waits,
                "the listed back edge must wait on every taken iteration and re-read RunQueueBits "
                "after every wait" + mode + " (" + std::to_string(waits) + " waits, " +
                    std::to_string(reads) + " reads)");
        require(system.game_ticks() == armed + kIdleEdge + 1u && c.decrementer_pending() &&
                    c.interrupt_pending(),
                "the wait must stop the game clock at the event that made the interrupt "
                "deliverable" + mode);
        require(c.fallback_total(FallbackReason::Yield) == 1u &&
                    single_fallback(c, kIdleLoop, kIdleBackEdge, FallbackReason::Yield) &&
                    c.interpreter_exits == 1u && c.interpreted_instructions() == 0u,
                "the generated loop must hand back once, natively, from the listed back edge" + mode);
        const auto& report = c.verify_report();
        if (verify) {
            require(report.skipped_device == 1u && report.verified == 0u && report.mismatches == 0u &&
                        report.skipped_exit == 0u,
                    "a segment that reached a wait point must be skipped as device access, not replayed");
        } else {
            require(report.skipped_device == 0u && report.verified == 0u && report.mismatches == 0u,
                    "the plain run must not involve the oracle");
        }
        std::cout << "PASS SelectThread idle loop waits at its listed back edge" << mode << ": " << waits
                  << " waits, " << reads << " RunQueueBits reads, clock " << armed << " -> "
                  << system.game_ticks() << ", hand-back at 0x" << std::hex << c.pc << " from 0x"
                  << kIdleBackEdge << std::dec << '\n';
    }

    // A thread is ready: the condition holds on the first read, the back edge
    // is not taken and nothing waits. The segment from the loop head to the
    // per-call cut at the next call verifies against the interpreter, and the
    // clock does not move.
    {
        prepare(true, 0x00000001u);
        const Timeline before = timeline(c);
        const u64 waits_before = system.idle_waits();
        const u64 steps = c.run_steps(1u);
        const auto& report = c.verify_report();
        require(steps == 1u && !c.stopped() && c.pc != kIdleLoop, "the ready idle loop did not run: " +
                                                                       c.stop().detail);
        require(report.verified == 1u && report.mismatches == 0u && report.skipped_device == 0u &&
                    report.skipped_exit == 0u && report.verified_instructions >= 4u &&
                    c.fallback_total(FallbackReason::VerifyCut) == 1u &&
                    c.fallback_total(FallbackReason::Yield) == 0u,
                "with a thread ready the loop must leave without waiting and its segment must verify");
        require(system.idle_waits() == waits_before && timeline(c) == before && !c.decrementer_pending(),
                "a loop whose condition already holds must not wait or move the game clock");
        std::cout << "PASS SelectThread idle loop with a ready thread: no wait, one verified segment of "
                  << report.verified_instructions << " instructions up to the next call\n";
    }

    // No game clock owner (a bare Cpu, nothing installed by System): nothing
    // can deliver an event, so Cpu::idle_wait stops the machine and the back
    // edge's poll hands back at the loop head.
    {
        GuestMemory bus;
        Cpu bare(bus);
        bare.attach_recompiled(&code);
        bare.set_record_fallbacks(true);
        bare.set_entry(kIdleLoop);
        bare.gpr[13] = kSda;
        bare.set_msr(bare.msr() | kMsrEE);
        const u64 steps = bare.run_steps(1u);
        require(steps == 1u && bare.stopped() && bare.stop().reason == StopReason::ExecutionFault &&
                    bare.stop().detail.find("no game clock owner") != std::string::npos &&
                    bare.pc == kIdleLoop && bare.game_clock().now() == 0u &&
                    single_fallback(bare, kIdleLoop, kIdleBackEdge, FallbackReason::Yield),
                "an idle wait with no game clock owner must stop the machine at the loop head: " +
                    bare.stop().detail);
        std::cout << "PASS SelectThread idle loop without a game clock owner stops at its head\n";
    }
}
}
int main(int argc, char** argv) {
    RecompiledCode code;
    std::unique_ptr<System> system;
    try {
        require(argc == 2, "usage: compiler_body_tests <main.dol>");
        const auto dol = load(argv[1]);
        code.install(recomp::kFunctions, recomp::kFunctionCount);
        code.install_ranges(recomp::kRanges, recomp::kRangeCount);
        system = std::make_unique<System>();
        require(system->divide_intrinsics(), "the exact 64-bit divide intrinsics must default on");
        // This fixture verifies the recovered bodies themselves, so none of the
        // four helpers may be served natively here.
        system->set_divide_intrinsics(false);
        system->load_dol(dol); system->start();
        // The compiler intrinsics that served the tick wrapper and the TB->us
        // scaling were deleted with the instruction clock, and the divide
        // intrinsics are off: no host hook may serve any body verified here.
        for (u32 entry : {0x80051BBCu, 0x80051414u, 0x803A1F54u, 0x803A2040u, 0x803C5B2Cu, 0x803C5D8Cu,
                          0x803C607Cu})
            require(!system->cpu().has_hle(entry), "a host hook serves a guest body this fixture verifies");
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
    // Each body and the wait-point contract report on their own, so one
    // failure does not hide the state of the others.
    unsigned failures = 0;
    const auto run = [&failures](const std::string& name, const auto& body) {
        try { body(); }
        catch (const std::exception& e) {
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
            ++failures;
        }
    };
    for (u32 entry : {0x80051BBCu, 0x80051414u, 0x803A1F54u, 0x803A2040u})
        run("helper " + hex(entry), [&] { verify_compiler_helper(*system, code, entry); });
    for (u32 entry : {0x803C5B2Cu, 0x803C5D8Cu, 0x803C607Cu})
        run("audio " + hex(entry), [&] { verify_audio_decoder(*system, code, entry); });
    // The polling-loop contract that replaced the coalesced RAM poll, on the
    // generated SelectThread idle loop.
    run("wait points", [&] { wait_point_contract(*system, code); });
    return failures == 0u ? 0 : 1;
}
