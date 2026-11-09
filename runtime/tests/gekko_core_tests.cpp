// Regression tests for the Gekko interpreter and the Flipper device models.
//
// Every case here corresponds to behaviour that was observed to be required by
// the retail boot path, so a regression in any of them stops the game booting.

#include <algorithm>
#include "shadow/backends/storage_backends.h"
#include <cstdint>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "gekko/asset_mount.h"
#include "gekko/cpu.h"
#include "gekko/cpu_diag.h"
#include "gekko/os_exception_vector.h"
#include "gekko/display.h"
#include "gekko/recomp.h"
#include "gekko/gx.h"
#include "gekko/host_jobs.h"
#include "gekko/host_stats.h"
#include "gekko/host_window.h"
#include "gekko/guest_memory.h"
#include "gekko/native_immediate.h"
#include "gekko/ppc_instruction_traits.h"
#include "gekko/ppc_ops.h"
#include "gekko/savestate.h"
#include "gekko/appear_fade.h"
#include "gekko/system.h"
#include "support/fst_oracle.h"

namespace {

using namespace gekko;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void recomp_stub_a(Cpu&) {}
void recomp_stub_b(Cpu&) {}
void recomp_stub_c(Cpu&) {}
void recomp_stub_d(Cpu&) {}

void test_recompiled_code_refuses_entries_below_the_image() {
    // There is no real mode (docs/memory-architecture-20261006): an entry
    // below the program image - a GameCube real-mode alias such as the reset
    // vector 0x00000100 or __OSInitBATs at its physical address 0x00376A48 -
    // names nothing the game can run, and install() refuses it. Refused
    // entries must not stretch the dense span either: when 0x00376A48 once
    // landed in the dense table it dragged low_ down from ~0x80003100 and the
    // slot array committed 4.06 GiB.
    const RecompiledCode::Entry entries[] = {
        {0x00000100u, 4u, recomp_stub_a, "vector_real"},
        {0x00376A48u, 128u, recomp_stub_b, "OSInitBATs_Real"},
        {0x80003100u, 4u, recomp_stub_c, "first_translated"},
        {0x804AAC38u, 4u, recomp_stub_d, "last_translated"},
    };
    RecompiledCode code;
    code.install(entries, 4);
    require(code.lookup(0x00000100u) == nullptr, "an entry below the image is refused");
    require(code.lookup(0x00376A48u) == nullptr, "a physical alias is refused");
    require(code.lookup(0x80003100u) == recomp_stub_c, "first translated entry resolves");
    require(code.lookup(0x804AAC38u) == recomp_stub_d, "last translated entry resolves");
    require(code.lookup(0x80003104u) == nullptr, "absent translated address misses");
    require(code.low() >= GuestMemory::kImageBase, "a refused entry must not stretch the dense span");
    // The dense span covers ~4.7 MiB of addresses: well under 32 MiB of table.
    require(code.table_bytes() < 32u * 1024u * 1024u,
            "lookup tables stay within the expected footprint");
}

// Miniature translated functions in the exact shape tools/ppcrecomp emits:
// entry thunks call try_hle first, interior entry thunks do not, calls are
// direct native calls followed by the exiting/link checks (R12: the exiting
// check runs the callee's published transfers first), and blr is
// `c.pc = c.lr & ~3u; return;`. Guest program, a function calling a leaf:
//   0x80100000  mflr r0 ; addi r3,r3,1 ; bl 0x80100100
//   0x8010000C  addi r3,r3,2 ; mtlr r0 ; blr       (resume point after the bl)
//   0x80100100  addi r4,r4,5 ; blr
// Runs start with LR = kVerifyCutEnd (`b .`). The same instructions are
// written to MEM1 so --verify-recomp's interpreter replay executes the
// identical program.
constexpr u32 kVerifyCutA = 0x80100000u;
constexpr u32 kVerifyCutResume = 0x8010000Cu;
constexpr u32 kVerifyCutB = 0x80100100u;
constexpr u32 kVerifyCutEnd = 0x80100020u;
constexpr u32 kVerifyFpFault = 0x80100200u;
constexpr u32 kVerifyIllegalFault = 0x80100280u;

void verify_cut_fn_b_body(Cpu& c) {
    c.gpr[4] += 5u;
    c.pc = c.lr & ~3u;  // blr
}
void verify_cut_fn_b(Cpu& c) {
    if (c.try_hle(kVerifyCutB)) return;
    verify_cut_fn_b_body(c);
}
void verify_cut_fn_a_body(Cpu& c, u32 entry);
void verify_cut_fn_a(Cpu& c) {
    if (c.try_hle(kVerifyCutA)) return;
    verify_cut_fn_a_body(c, kVerifyCutA);
}
void verify_cut_fn_a_resume(Cpu& c) { verify_cut_fn_a_body(c, kVerifyCutResume); }
void verify_cut_fn_a_body(Cpu& c, u32 entry) {
    if (entry == kVerifyCutResume) goto resume;
    c.gpr[0] = c.lr;
    c.gpr[3] += 1u;
    c.lr = kVerifyCutResume;
    verify_cut_fn_b(c);
    if (c.exiting() && c.finish_transfers()) return;
    if (c.pc != kVerifyCutResume) {
        recomp::resume_elsewhere(c, kVerifyCutA + 8u);
        return;
    }
resume:
    c.gpr[3] += 2u;
    c.lr = c.gpr[0];
    c.pc = c.lr & ~3u;  // blr
}

void write_verify_cut_program(GuestMemory& memory) {
    memory.write32(kVerifyCutA, 0x7C0802A6u);              // mflr r0
    memory.write32((kVerifyCutA + 4u), 0x38630001u);       // addi r3,r3,1
    memory.write32((kVerifyCutA + 8u), 0x480000F9u);       // bl 0x80100100
    memory.write32(kVerifyCutResume, 0x38630002u);         // addi r3,r3,2
    memory.write32((kVerifyCutResume + 4u), 0x7C0803A6u);  // mtlr r0
    memory.write32((kVerifyCutResume + 8u), 0x4E800020u);  // blr
    memory.write32(kVerifyCutEnd, 0x48000000u);            // b .
    memory.write32(kVerifyCutB, 0x38840005u);              // addi r4,r4,5
    memory.write32((kVerifyCutB + 4u), 0x4E800020u);       // blr
}

const RecompiledCode::Entry kVerifyCutEntries[] = {
    {kVerifyCutA, 24u, verify_cut_fn_a, "verify_cut_a"},
    {kVerifyCutResume, 12u, verify_cut_fn_a_resume, "verify_cut_a_resume"},
    {kVerifyCutB, 8u, verify_cut_fn_b, "verify_cut_b"},
};

void verify_fp_fault(Cpu& c) {
    if (c.try_hle(kVerifyFpFault)) return;
    recomp::fp_unavailable(c, kVerifyFpFault);
}

// The generator's guard for dcbz_l 0,r4: LCE is the one HID2 enable still
// tested at run time (audit paired-single-10, change C).
void verify_illegal_fault(Cpu& c) {
    if (c.try_hle(kVerifyIllegalFault)) return;
    if ((c.spr[920] & 0x10000000u) != 0x10000000u) {
        recomp::illegal_instruction(c, kVerifyIllegalFault);
        return;
    }
    const u32 ea = c.gpr[4] & ~31u;
    for (u32 i = 0; i < 32u; i += 4u) { c.mem_write32(ea + i, 0u); }
    c.leave_to_dispatcher(kVerifyIllegalFault + 4u);
}

void test_host_jobs_partition_and_join_deterministically() {
    std::vector<u32> output(4096u, 0u);
    const HostJobReport before = host_job_report();
    for (u32 pass = 0; pass < 3u; ++pass) {
        host_parallel_for(gekko::JobSite::Unknown, 0, static_cast<int>(output.size()), [&](int index) {
            const u32 value = static_cast<u32>(index);
            output[static_cast<std::size_t>(index)] =
                (value * 2654435761u) ^ (pass * 0x9e3779b9u);
        });
        for (std::size_t index = 0; index < output.size(); ++index) {
            const u32 value = static_cast<u32>(index);
            require(output[index] ==
                        ((value * 2654435761u) ^ (pass * 0x9e3779b9u)),
                    "host jobs must join before deterministic output is observed");
        }
    }
    const HostJobReport after = host_job_report();
    if (after.logical_workers > 1u) {
        require(after.parallel_groups >= before.parallel_groups + 3u,
                "large independent ranges must use the persistent host pool");
        require(after.worker_chunks > before.worker_chunks,
                "host pool workers must execute part of a parallel group");
        require(after.caller_chunks >= before.caller_chunks + 3u,
                "the guest thread must participate without surrendering its core");
    }

    bool propagated = false;
    try {
        host_parallel_for(gekko::JobSite::Unknown, 0, 64, [](int index) {
            if (index == 37) throw std::runtime_error("host job test");
        });
    } catch (const std::runtime_error&) {
        propagated = true;
    }
    require(propagated, "worker failures must cross the group barrier on the caller thread");
}

// Writes a sequence of instructions at the guest address and runs them.
class Fixture {
public:
    Fixture() : cpu(bus) {
        cpu.reset();
        cpu.set_entry(kBase);
    }

    void emit(std::initializer_list<u32> words) {
        u32 address = kBase;
        for (const u32 word : words) {
            bus.write32(address, word);
            address += 4;
        }
        // A trailing branch-to-self bounds the run.
        bus.write32(address, 0x48000000u);
        cpu.set_entry(kBase);
    }

    // Dispatches: one interpreted instruction or one native call tree each.
    u64 run(u64 count) { return cpu.run_steps(count); }

    // Where the game's own text starts: below it, 0x80003000 is
    // __OSExceptionTable, which the native OS exception entry reads.
    static constexpr u32 kBase = 0x80003100u;
    GuestMemory bus;
    Cpu cpu;
};

// The OS state the native exception entry (os_exception_entry.cpp) consumes,
// as __OSExceptionInit and OSSetCurrentContext leave it: the vector page holds
// the template (gekko/os_exception_vector.h) with
// `li r3, <index>` patched at +0x68, __OSExceptionTable (0x80003000) names the
// handler, low memory 0xD4 the current OSContext.
constexpr u32 kTestContext = 0x80006000u;
void install_os_exception(GuestMemory& memory, Exception vector, u32 index, u32 handler) {
    // The whole page: for the system-call vector the first 7 words are PPCSync's head (os_exception_page_word).
    for (u32 word = 0; word < kOsVectorWords; ++word)
        memory.write32((0x80000000u | static_cast<u32>(vector)) + 4u * word, os_exception_page_word(word, index));
    memory.write32(0x80003000u + 4u * index, handler);
    memory.write32(0x800000D4u, kTestContext);
}

// Records the lowered GX sites' calls (gekko/gx_sites.h) as text.
struct RecordingGxSink final : GxSiteSink {
    std::vector<std::string> calls;
    void record(const char* what, u32 site, u32 a, u32 b) {
        char line[96];
        std::snprintf(line, sizeof(line), "%s %08X %X %X", what, site, a, b);
        calls.emplace_back(line);
    }
    void draw_sync_token(u32 site, u16 token, bool interrupt) override { record("token", site, token, interrupt); }
    void draw_done(u32 site, u32 value) override { record("draw_done", site, value, 0u); }
    void copy_trigger(u32 site, u32 value) override { record("copy", site, value, 0u); }
    void primitive_opcode(u32 site, u8 opcode) override { record("opcode", site, opcode, 0u); }
    void primitive_count(u32 site, u16 count) override { record("count", site, count, 0u); }
    void vertex(u32 site, u32 value, u32 bytes) override { record("vertex", site, value, bytes); }
};

void test_fp_unavailable_is_shared_by_interpreter_and_verifier() {
    constexpr u32 lfs = (48u << 26) | (1u << 21) | (3u << 16);
    constexpr u32 lfsx = (31u << 26) | (1u << 21) | (3u << 16) |
                         (4u << 11) | (535u << 1);
    constexpr u32 dcbz_l = (4u << 26) | (1014u << 1);
    require(instruction_requires_fp(lfs), "scalar FP loads require MSR[FP]");
    require(instruction_requires_fp(lfsx), "indexed scalar FP loads require MSR[FP]");
    require(!instruction_requires_fp(dcbz_l), "dcbz_l is not an FP instruction");

    constexpr u32 kFpHandler = 0x80005000u;
    Fixture direct;
    direct.emit({lfs});
    install_os_exception(direct.bus, Exception::FpUnavailable, 7u, kFpHandler);
    direct.cpu.set_msr(direct.cpu.msr() & (~kMsrFP));
    direct.cpu.gpr[3] = 0x80004000u;
    direct.cpu.step();
    require(direct.cpu.interpreted_instructions() == 0u,
            "FP-unavailable must fault before the instruction completes");
    require(direct.cpu.pc == kFpHandler && direct.cpu.gpr[3] == 7u && direct.cpu.gpr[4] == kTestContext &&
                direct.bus.read32(kTestContext + 0x198u) == Fixture::kBase &&
                direct.bus.read32(kTestContext + 0x0Cu) == 0x80004000u &&
                (direct.cpu.msr() & (kMsrIR | kMsrDR)) == (kMsrIR | kMsrDR),
            "interpreter FP-unavailable must enter the OS handler natively with the fault saved in the context");

    Fixture verified;
    verified.bus.write32(kVerifyFpFault, lfs);
    install_os_exception(verified.bus, Exception::FpUnavailable, 7u, kFpHandler);
    verified.bus.write32(kFpHandler, 0x38630001u);  // addi r3,r3,1
    const RecompiledCode::Entry entries[] = {
        {kVerifyFpFault, 4u, verify_fp_fault, "verify_fp_fault"}};
    RecompiledCode code;
    code.install(entries, 1u);
    verified.cpu.attach_recompiled(&code);
    verified.cpu.set_verify_recompiled(true);
    verified.cpu.set_entry(kVerifyFpFault);
    verified.cpu.set_msr(verified.cpu.msr() & (~kMsrFP));
    const u64 exceptions_before = cpu_diag().exceptions_total;
    const u64 steps = verified.cpu.run_steps(2u);
    require(steps == 2u && verified.cpu.gpr[3] == 8u,
            "the OS handler (entered with r3 = 7) must run after the verified fault");
    require(verified.cpu.verify_report().verified == 1u &&
                verified.cpu.verify_report().mismatches == 0u,
            "native and interpreter FP-unavailable transitions must match");
    require(cpu_diag().exceptions_total == exceptions_before + 1u,
            "discarded native verification must not double-count the exception");
}

// Audit paired-single-10 / change C: HID2 LSQE and PSE are machine
// invariants - the reset value has them and Cpu::write_hid2, the one path an
// mtspr (generated or interpreted) writes HID2 through, refuses a value that
// would clear either - so neither engine tests them. LCE stays a run-time
// enable: dcbz_l with LCE = 0 is the illegal-instruction program exception
// (gekko_user_manual.txt PAGE 90; 0x700, SRR0 at the fault, SRR1 bit 12,
// nothing retired), and the interpreter and the verifier's zero-retirement
// replay must agree with the generated guard.
void test_illegal_extension_is_shared_by_interpreter_and_verifier() {
    constexpr u32 dcbz_l = (4u << 26) | (3u << 11) | (1014u << 1);  // dcbz_l 0,r3
    constexpr u32 ps_add = (4u << 26) | (1u << 21) | (2u << 16) | (3u << 11) | (21u << 1);
    constexpr u32 mtspr_hid2_r4 =
        (31u << 26) | (4u << 21) | ((920u & 0x1fu) << 16) | ((920u >> 5) << 11) | (467u << 1);
    require(instruction_required_hid2(dcbz_l) == kHid2LCE, "dcbz_l needs HID2[LCE]");
    require(instruction_required_hid2(ps_add) == kHid2PSE, "ps_add needs HID2[PSE]");
    require(instruction_checked_hid2(dcbz_l) == kHid2LCE, "LCE is the run-time HID2 enable");
    require(instruction_checked_hid2(ps_add) == 0u, "PSE is a machine invariant, never tested");

    constexpr u32 kProgramHandler = 0x80005100u;
    Fixture direct;
    direct.emit({dcbz_l});
    install_os_exception(direct.bus, Exception::Program, 6u, kProgramHandler);
    direct.cpu.gpr[3] = 0x80004000u;
    direct.bus.write32(0x80004000u, 0x11111111u);
    direct.cpu.step();  // reset HID2 = 0xE0000000: LCE clear
    require(direct.cpu.interpreted_instructions() == 0u && direct.bus.read32(0x80004000u) == 0x11111111u,
            "illegal dcbz_l must fault before completing or zeroing");
    require(direct.cpu.pc == kProgramHandler && direct.cpu.gpr[3] == 6u &&
                direct.bus.read32(kTestContext + 0x198u) == Fixture::kBase &&
                (direct.bus.read32(kTestContext + 0x19Cu) & kSrr1IllegalInstruction) != 0u,
            "illegal dcbz_l must enter the OS program handler with the fault and SRR1 bit 12 in the context");

    Fixture refused;
    refused.emit({mtspr_hid2_r4});
    refused.cpu.gpr[4] = kHid2LCE;  // LCE alone would clear LSQE|PSE
    refused.cpu.step();
    require(refused.cpu.stopped() && refused.cpu.spr[SPR_HID2] == 0xe0000000u,
            "an mtspr HID2 clearing LSQE|PSE must be refused and leave HID2 unchanged");

    Fixture accepted;
    accepted.emit({mtspr_hid2_r4});
    accepted.cpu.gpr[4] = 0xf0000000u;
    accepted.cpu.step();
    require(!accepted.cpu.stopped() && accepted.cpu.spr[SPR_HID2] == 0xf0000000u &&
                accepted.cpu.interpreted_instructions() == 1u,
            "an mtspr HID2 keeping LSQE|PSE must be accepted");

    Fixture translated;
    translated.cpu.write_hid2(kHid2PSE);
    require(translated.cpu.stopped() && translated.cpu.spr[SPR_HID2] == 0xe0000000u,
            "the translated mtspr path must refuse the same write");

    constexpr u32 dcbz_l_r4 = (4u << 26) | (4u << 11) | (1014u << 1);  // dcbz_l 0,r4
    Fixture verified;
    verified.bus.write32(kVerifyIllegalFault, dcbz_l_r4);
    install_os_exception(verified.bus, Exception::Program, 6u, kProgramHandler);
    verified.bus.write32(kProgramHandler, 0x38630001u);  // addi r3,r3,1
    const RecompiledCode::Entry entries[] = {
        {kVerifyIllegalFault, 4u, verify_illegal_fault, "verify_illegal_fault"}};
    RecompiledCode code;
    code.install(entries, 1u);
    verified.cpu.attach_recompiled(&code);
    verified.cpu.set_verify_recompiled(true);
    verified.cpu.set_entry(kVerifyIllegalFault);
    verified.cpu.gpr[4] = 0x80004000u;  // reset HID2 = 0xE0000000: LCE clear
    const u64 steps = verified.cpu.run_steps(2u);
    require(steps == 2u && verified.cpu.gpr[3] == 7u,
            "the OS program handler (entered with r3 = 6) must run after the verified fault");
    require(verified.cpu.verify_report().verified == 1u &&
                verified.cpu.verify_report().mismatches == 0u,
            "native and interpreter illegal-instruction transitions must match");
}

void test_write_journal_traces_each_guest_access_once() {
    GuestMemory bus;
    u64 traced = 0;
    bus.set_access_trace([&](u32, std::size_t, bool) { ++traced; });
    bus.write32(0x80001000u, 0x12345678u);
    traced = 0;

    bus.journal_begin();
    require(bus.read32(0x80001000u) == 0x12345678u, "journalled RAM read value");
    require(traced == 1u, "one journalled guest read must be traced once");
    require(bus.journal().empty(), "a journalled read records nothing");
    bus.write32(0x80001000u, 0x89abcdefu);
    require(traced == 2u, "journal capture must not re-trace a guest write");
    require(bus.journal().size() == 1u && bus.journal()[0].address == 0x80001000u &&
                bus.journal()[0].size == 4u && bus.journal()[0].previous == 0x12345678u,
            "a journalled write records the bytes it replaced");
    require(!bus.journal_touched_device(), "a RAM write is not a device access");
    bus.journal_rollback();
    require(traced == 2u, "journal rollback must not look like a guest RAM access");
    require(bus.journal().empty(), "rollback consumes the journal");
    bus.journal_end();
    bus.set_access_trace({});
    require(bus.read32(0x80001000u) == 0x12345678u, "journal rollback restores bytes");
}

// The resident-copy footprint reconstruction must never apply to a magnifying
// draw; the CPU decides that from this footprint, so pin its three regimes on
// the glare ladder's own geometry (guest pixel-centre quads, normalised UVs).
void test_texel_footprint_regimes() {
    const auto quad_footprint = [](float x0, float y0, float x1, float y1, u32 w, u32 h) {
        return texel_footprint(x0, y0, 0.0f, 0.0f, x1, y0, 1.0f, 0.0f, x0, y1, 0.0f, 1.0f, w, h);
    };
    const auto near = [](float a, float b) { return std::fabs(a - b) < 1.0e-4f; };
    require(near(quad_footprint(0.5f, 0.5f, 128.5f, 128.5f, 128u, 128u), 1.0f),
            "a 1:1 ladder blur tap spans exactly one texel per pixel");
    require(near(quad_footprint(0.5f, 0.5f, 128.5f, 128.5f, 640u, 480u), 3.75f),
            "the 640x480 -> 128x128 downsample minifies (less minified axis 480/128)");
    require(near(quad_footprint(0.5f, 0.5f, 640.5f, 480.5f, 128u, 128u), 128.0f / 640.0f),
            "the 128x128 -> 640x480 bloom composite magnifies");
    require(texel_footprint(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 2.0f, 2.0f, 0.5f,
                            0.5f, 64u, 64u) == 0.0f,
            "a degenerate triangle has no footprint");
}


void start_verify_cut(Fixture& fixture, RecompiledCode& code, bool verify) {
    write_verify_cut_program(fixture.bus);
    fixture.cpu.attach_recompiled(&code);
    fixture.cpu.set_verify_recompiled(verify);
    fixture.cpu.lr = kVerifyCutEnd;
    fixture.cpu.set_entry(kVerifyCutA);
}

void test_verify_recomp_leaves_the_game_clock_alone() {
    // No time passes inside guest code, in either engine. The oracle rolls
    // memory and registers back for its replay but never the clock, so a
    // verified run must leave the clock - now and every deadline - exactly as
    // a plain run does, and both exactly as they found it.
    RecompiledCode code;
    code.install(kVerifyCutEntries, 3);
    for (unsigned mode = 0; mode < 2; ++mode) {
        Fixture fixture;
        start_verify_cut(fixture, code, mode != 0u);
        fixture.cpu.set_decrementer(1000u);
        const u64 now = fixture.cpu.game_clock().now();
        const auto deadlines = fixture.cpu.game_clock().deadlines();
        fixture.run(mode != 0u ? 3u : 1u);
        require(fixture.cpu.gpr[3] == 3u && fixture.cpu.gpr[4] == 5u &&
                    fixture.cpu.pc == kVerifyCutEnd,
                "both arms must complete the program");
        require(fixture.cpu.verify_report().mismatches == 0, "the replays agree");
        require(fixture.cpu.game_clock().now() == now &&
                    fixture.cpu.game_clock().deadlines() == deadlines &&
                    fixture.cpu.decrementer() == 1000u,
                "guest code, verified or not, must not move the game clock");
    }
}

// A wrong translation of `addi r3,r3,1 ; stw r4,0(r5) ; blr` that returns
// without its store.
void verify_skips_store(Cpu& c) {
    if (c.try_hle(kVerifyCutA)) return;
    ++c.gpr[3];
    c.pc = c.lr & ~3u;
}

void test_verify_recomp_mismatch_keeps_the_reference_and_stops() {
    Fixture fixture;
    const RecompiledCode::Entry entries[] = {
        {kVerifyCutA, 12u, verify_skips_store, "verify_skips_store"}};
    RecompiledCode code;
    code.install(entries, 1);
    fixture.cpu.attach_recompiled(&code);
    fixture.cpu.set_verify_recompiled(true);
    fixture.bus.write32(kVerifyCutA, 0x38630001u);         // addi r3,r3,1
    fixture.bus.write32((kVerifyCutA + 4u), 0x90850000u);  // stw r4,0(r5)
    fixture.bus.write32((kVerifyCutA + 8u), 0x4E800020u);  // blr
    fixture.bus.write32(kVerifyCutEnd, 0x48000000u);       // b .
    fixture.cpu.gpr[4] = 0xdeadbeefu;
    fixture.cpu.gpr[5] = 0x80110000u;
    fixture.bus.write32(0x80110000u, 0x12345678u);
    fixture.cpu.lr = kVerifyCutEnd;
    fixture.cpu.set_entry(kVerifyCutA);
    fixture.cpu.set_decrementer(1000u);
    const u64 now = fixture.cpu.game_clock().now();
    require(fixture.run(4) == 1u, "the failed segment is the run's last dispatch");
    const Cpu::VerifyReport& report = fixture.cpu.verify_report();
    require(report.mismatches == 1u && !report.mismatch_messages.empty() &&
                report.mismatch_messages[0].find("written by the interpreter only") != std::string::npos,
            "the dropped store must be diagnosed");
    require(fixture.cpu.stop().reason == StopReason::ExecutionFault, "a failed oracle must stop the run");
    require(fixture.cpu.pc == kVerifyCutEnd && fixture.cpu.gpr[3] == 1u &&
                fixture.bus.read32(0x80110000u) == 0xdeadbeefu,
            "the machine keeps the interpreter's (reference) result of the segment");
    require(fixture.cpu.game_clock().now() == now && fixture.cpu.decrementer() == 1000u,
            "a mismatch diagnostic must not move the game clock");
    require(fixture.run(4) == 0u && fixture.cpu.pc == kVerifyCutEnd,
            "nothing runs after the failed segment until the stop is cleared");
}


void test_verify_recomp_unrecognisable_end_is_skipped() {
    // A native pass that hands back without returning, faulting or reaching a
    // call - here at the trap vector without raising the exception - has no
    // end the replay can recognise without an instruction count. It is not
    // verified, the native result stands, and verification does not loop on
    // it.
    Fixture fixture;
    RecompiledCode code;
    const RecompiledCode::Entry entries[] = {
        {kVerifyCutA, 4u, +[](Cpu& c) {
            c.leave_to_dispatcher(0x800u, FallbackReason::Trap, kVerifyCutA);
        }, "unrecognisable-end"}
    };
    code.install(entries, 1);
    fixture.cpu.attach_recompiled(&code);
    fixture.cpu.set_verify_recompiled(true);
    fixture.bus.write32(kVerifyCutA, 0x38630001u);
    fixture.cpu.set_entry(kVerifyCutA);
    const u64 steps = fixture.run(1);
    const Cpu::VerifyReport& report = fixture.cpu.verify_report();
    require(steps == 1u && report.skipped_exit == 1u && report.verified == 0u &&
                report.mismatches == 0u,
            "an end the replay cannot recognise is skipped, not verified or retried");
    require(!fixture.cpu.stopped() && fixture.cpu.pc == 0x800u && fixture.cpu.gpr[3] == 0u,
            "the native result stands");
}

void test_verify_recomp_per_call_granularity() {
    // The differential oracle must verify each guest call as its own segment
    // even though calls are direct native-to-native transfers. Before the
    // per-call cut, this route would have been one dispatch and one verified
    // "call" spanning everything; a 400M-instruction boot route degraded to
    // 599 verified fragments with 34 device-touching subtrees carrying
    // essentially every instruction unverified. Each segment ends where the
    // replay can recognise it without an instruction count: the caller at the
    // call it was cut at, the leaf at its return, the caller's resume at its
    // own return.
    RecompiledCode code;
    code.install(kVerifyCutEntries, 3);

    Fixture verified;
    start_verify_cut(verified, code, true);
    const u64 steps = verified.run(3);
    const Cpu::VerifyReport& report = verified.cpu.verify_report();
    require(steps == 3u && report.verified == 3,
            "the caller up to its call, the callee and the caller's resume must verify as "
            "separate segments");
    require(report.mismatches == 0, "the two engines must agree on every segment");
    require(report.skipped_device == 0 && report.skipped_exit == 0,
            "no segment of this register-only program may be skipped");
    require(report.verified_instructions == 8u,
            "the replays compared every instruction: mflr addi bl | addi blr | addi mtlr blr");
    require(verified.cpu.fallback_total(FallbackReason::VerifyCut) == 1,
            "the nested direct call must hand back exactly once");
    require(verified.cpu.gpr[3] == 3u && verified.cpu.gpr[4] == 5u &&
                verified.cpu.gpr[0] == kVerifyCutEnd,
            "the verified run must leave the program's results in place");
    require(verified.cpu.pc == kVerifyCutEnd, "the verified run must end where the program ends");

    // The identical route without verification: no cuts, identical result.
    Fixture plain;
    start_verify_cut(plain, code, false);
    plain.run(1);
    require(plain.cpu.fallback_total(FallbackReason::VerifyCut) == 0,
            "verification cuts must not exist outside --verify-recomp");
    require(plain.cpu.gpr[3] == 3u && plain.cpu.gpr[4] == 5u && plain.cpu.pc == kVerifyCutEnd,
            "normal execution must be unchanged by the verification machinery");
}

// ---- 2026-09-13 recompiler-core audit, batch b1 (oracle + interpreter) -------

bool verify_message_contains(const Cpu& cpu, const char* needle) {
    for (const std::string& row : cpu.verify_report().mismatch_messages) {
        if (row.find(needle) != std::string::npos) return true;
    }
    return false;
}

// memory-model-1: stb r5,0(r4) ; stw r6,0(r4) with r4 = 0xCC008000.


// oracle-reference-2: native translation of stw r4,0(r3) that drops the store.
// oracle-reference-2: native translation of `stw r4,0(r3) ; blr` that drops
// the store.
void verify_dropped_mmio_store(Cpu& c) {
    if (c.try_hle(kVerifyCutA)) return;
    c.pc = c.lr & ~3u;
}

void test_verify_recomp_reports_interpreter_only_device_access() {
    Fixture fixture;
    fixture.bus.set_trace_unmapped(false);
    fixture.bus.write32(kVerifyCutA, 0x90830000u);         // stw r4,0(r3)
    fixture.bus.write32((kVerifyCutA + 4u), 0x4E800020u);  // blr
    fixture.bus.write32(kVerifyCutEnd, 0x48000000u);       // b .
    fixture.cpu.lr = kVerifyCutEnd;
    const RecompiledCode::Entry entries[] = {
        {kVerifyCutA, 8u, verify_dropped_mmio_store, "verify_dropped_mmio_store"}};
    RecompiledCode code;
    code.install(entries, 1u);
    fixture.cpu.attach_recompiled(&code);
    fixture.cpu.set_verify_recompiled(true);
    fixture.cpu.gpr[3] = 0xCC003000u;  // unmapped hardware window in this fixture
    fixture.cpu.gpr[4] = 0x12345678u;
    fixture.cpu.set_entry(kVerifyCutA);
    fixture.run(1u);
    const Cpu::VerifyReport& report = fixture.cpu.verify_report();
    require(report.mismatches == 1u && report.verified == 0u,
            "a hardware store only the interpreter performed must be a mismatch");
    require(verify_message_contains(fixture.cpu, "interpreter touched hardware"),
            "the mismatch must name the interpreter-only hardware access");
    require(fixture.cpu.stop().reason == StopReason::ExecutionFault,
            "a failed oracle must stop the run");
}

// oracle-reference-3: stfd f0,16(r1) translated as a 4-byte store of the high word.
// oracle-reference-3: `stfd f0,16(r1) ; blr` translated as a 4-byte store of
// the high word.
void verify_truncated_stfd(Cpu& c) {
    if (c.try_hle(kVerifyCutA)) return;
    c.mem_write32(c.gpr[1] + 16u, static_cast<u32>(c.fpr[0].bits[0] >> 32));
    c.pc = c.lr & ~3u;
}

void test_verify_recomp_checks_every_interpreter_written_byte() {
    Fixture fixture;
    fixture.bus.write32(kVerifyCutA, 0xD8010010u);         // stfd f0,16(r1)
    fixture.bus.write32((kVerifyCutA + 4u), 0x4E800020u);  // blr
    fixture.bus.write32(kVerifyCutEnd, 0x48000000u);       // b .
    fixture.cpu.lr = kVerifyCutEnd;
    fixture.bus.write32(0x80400010u, 0xffffffffu);
    fixture.bus.write32(0x80400014u, 0xffffffffu);
    const RecompiledCode::Entry entries[] = {
        {kVerifyCutA, 8u, verify_truncated_stfd, "verify_truncated_stfd"}};
    RecompiledCode code;
    code.install(entries, 1u);
    fixture.cpu.attach_recompiled(&code);
    fixture.cpu.set_verify_recompiled(true);
    fixture.cpu.gpr[1] = 0x80400000u;
    fixture.cpu.fpr[0].bits[0] = 0x3FF0000000000000ull;  // leading four bytes 3F F0 00 00
    fixture.cpu.set_entry(kVerifyCutA);
    fixture.run(1u);
    require(fixture.cpu.verify_report().mismatches == 1u &&
                fixture.cpu.verify_report().verified == 0u,
            "a native store narrower than the interpreter's must not verify");
    require(verify_message_contains(fixture.cpu, "memory 0x80400014 written by the interpreter only"),
            "the first uncovered byte of the 8-byte interpreter store must be reported");
}

// load-store-3: lwarx translated as a plain lwzx (the pre-fix emitter). The
// reservation is architected state, so the oracle must see the difference.
// load-store-3: `lwarx r6,0,r3 ; blr` translated as a plain lwzx (the pre-fix
// emitter). The reservation is architected state, so the oracle must see the
// difference.
void verify_lwarx_without_reservation(Cpu& c) {
    if (c.try_hle(kVerifyCutA)) return;
    c.gpr[6] = c.mem_read32(c.gpr[3]);
    c.pc = c.lr & ~3u;
}

void test_verify_recomp_compares_reservation() {
    Fixture fixture;
    fixture.bus.write32(kVerifyCutA, 0x7CC01828u);         // lwarx r6,0,r3
    fixture.bus.write32((kVerifyCutA + 4u), 0x4E800020u);  // blr
    fixture.bus.write32(kVerifyCutEnd, 0x48000000u);       // b .
    fixture.cpu.lr = kVerifyCutEnd;
    fixture.bus.write32(0x80400020u, 0x13572468u);
    const RecompiledCode::Entry entries[] = {
        {kVerifyCutA, 8u, verify_lwarx_without_reservation, "verify_lwarx_without_reservation"}};
    RecompiledCode code;
    code.install(entries, 1u);
    fixture.cpu.attach_recompiled(&code);
    fixture.cpu.set_verify_recompiled(true);
    fixture.cpu.gpr[3] = 0x80400024u;
    fixture.cpu.set_entry(kVerifyCutA);
    fixture.run(1u);
    require(fixture.cpu.verify_report().mismatches == 1u &&
                fixture.cpu.verify_report().verified == 0u,
            "a native lwarx that sets no reservation must not verify");
    require(verify_message_contains(fixture.cpu, "reservation="),
            "the mismatch must name the reservation flag");
}

// oracle-reference-4: a hooked callee. kVerifyCutB (addi r4,r4,5 ; blr) gets a
// handler; the caller and resume are the per-call fixture above.
int g_hook_calls = 0;
bool g_hook_writes_ram = false;
u32 g_hook_serves_with = 0;  // 0 declines; otherwise serves adding this to r4

bool counting_hook(Cpu& c) {
    ++g_hook_calls;
    if (g_hook_writes_ram) {
        // A declining handler with RAM effects, like the AX voice advance.
        c.mem_write32(0x80110000u, static_cast<u32>(g_hook_calls));
    }
    if (g_hook_serves_with == 0u) return false;
    c.gpr[4] += g_hook_serves_with;  // the modelled addi ; blr (the call resumes at LR)
    return true;
}

void verify_cut_fn_b_at(Cpu& c, u32 entry) {
    (void)entry;
    verify_cut_fn_b_body(c);
}
void verify_cut_fn_a_at(Cpu& c, u32 entry) { verify_cut_fn_a_body(c, entry); }

const RecompiledCode::Range kVerifyCutRanges[] = {
    {kVerifyCutA, kVerifyCutA + 24u, verify_cut_fn_a_at},
    {kVerifyCutB, kVerifyCutB + 8u, verify_cut_fn_b_at},
};

void test_verify_recomp_runs_hle_handlers_once() {
    RecompiledCode code;
    code.install(kVerifyCutEntries, 3);
    code.install_ranges(kVerifyCutRanges, 2);

    // Declining handler, host state only and with a RAM write. Before the fix
    // the handler ran in the caller's pass, in the dispatcher's hook lambda
    // and again in the replay (3 calls), and a RAM write made in the caller's
    // discarded pass was a false "memory ... interpreter X native Y" stop.
    for (bool writes_ram : {false, true}) {
        g_hook_calls = 0;
        g_hook_writes_ram = writes_ram;
        g_hook_serves_with = 0;
        Fixture fixture;
        fixture.cpu.install_hle(kVerifyCutB, counting_hook, "counting hook");
        start_verify_cut(fixture, code, true);
        fixture.run(3u);
        const Cpu::VerifyReport& report = fixture.cpu.verify_report();
        require(g_hook_calls == 1, "a declining handler must run exactly once per guest call "
                                   "under --verify-recomp");
        require(report.mismatches == 0u && !fixture.cpu.stopped(),
                "a declining handler's effects must not produce a verification mismatch");
        require(report.verified == 3u && report.skipped_device == 0u && report.skipped_exit == 0u,
                "the caller up to its call, the hooked body and the caller's resume must verify");
        require(fixture.cpu.gpr[3] == 3u && fixture.cpu.gpr[4] == 5u &&
                    fixture.cpu.pc == kVerifyCutEnd,
                "the verified route must keep the program's results");
        require(!writes_ram || fixture.bus.read32(0x80110000u) == 1u,
                "the handler's RAM write is committed exactly once");
    }

    // Serving handler: host code, not translated code. It runs once and is
    // reported as an unverified HLE call instead of being "verified" against
    // itself (the old replay re-ran it in step()). Its semantics are the
    // handler's own - here deliberately r4 += 6, unlike the guest's addi 5 -
    // so no engine comparison may involve it.
    for (u32 serves_with : {5u, 6u}) {
        g_hook_calls = 0;
        g_hook_writes_ram = false;
        g_hook_serves_with = serves_with;
        Fixture fixture;
        fixture.cpu.install_hle(kVerifyCutB, counting_hook, "counting hook");
        start_verify_cut(fixture, code, true);
        fixture.run(3u);
        const Cpu::VerifyReport& report = fixture.cpu.verify_report();
        require(g_hook_calls == 1, "a serving handler must run exactly once per guest call");
        require(report.mismatches == 0u && !fixture.cpu.stopped(),
                "a served HLE call is not an engine comparison");
        require(report.skipped_hle == 1u && report.verified == 2u &&
                    report.verified_instructions == 6u,
                "the served call is reported as unverified HLE; the caller's two segments verify");
        require(fixture.cpu.gpr[4] == serves_with && fixture.cpu.gpr[3] == 3u &&
                    fixture.cpu.pc == kVerifyCutEnd,
                "the served route must keep the handler's and the program's results");
    }
    g_hook_serves_with = 0;
    g_hook_writes_ram = false;
}

// oracle-reference-6: F = addi r3,r3,1 ; addi r3,r3,2 ; addi r3,r3,4 ;
// addi r3,r3,8 ; blr. A dispatch at F+4 enters the translation mid-body;
// g_mid_wrong models a wrong cold entry that also runs F's first instruction.
constexpr u32 kVerifyMidF = 0x80100300u;
bool g_mid_wrong = false;

void verify_mid_fn(Cpu& c) {
    if (c.try_hle(kVerifyMidF)) return;
    c.gpr[3] += 15u;
    c.pc = c.lr & ~3u;
}

void verify_mid_fn_at(Cpu& c, u32 entry) {
    if (entry == kVerifyMidF) {
        verify_mid_fn(c);
        return;
    }
    if (entry == kVerifyMidF + 8u) {
        // A cold entry that cannot run its first instruction: hand back with
        // nothing done, which the dispatcher must answer with one step().
        c.leave_to_dispatcher(entry, FallbackReason::NotRecompiled, entry);
        return;
    }
    // entry == F+4: the cold mid-block entry.
    if (g_mid_wrong) c.gpr[3] += 1u;
    c.gpr[3] += 2u + 4u + 8u;
    c.pc = c.lr & ~3u;
}

void test_verify_recomp_covers_mid_function_dispatch() {
    const RecompiledCode::Entry entries[] = {{kVerifyMidF, 20u, verify_mid_fn, "verify_mid_fn"}};
    const RecompiledCode::Range ranges[] = {{kVerifyMidF, kVerifyMidF + 20u, verify_mid_fn_at}};
    RecompiledCode code;
    code.install(entries, 1u);
    code.install_ranges(ranges, 1u);
    const auto prepare = [&](Fixture& fixture, u32 entry) {
        fixture.bus.write32(kVerifyMidF, 0x38630001u);         // addi r3,r3,1
        fixture.bus.write32((kVerifyMidF + 4u), 0x38630002u);  // addi r3,r3,2
        fixture.bus.write32((kVerifyMidF + 8u), 0x38630004u);  // addi r3,r3,4
        fixture.bus.write32((kVerifyMidF + 12u), 0x38630008u); // addi r3,r3,8
        fixture.bus.write32((kVerifyMidF + 16u), 0x4E800020u); // blr
        fixture.bus.write32(kVerifyCutEnd, 0x48000000u);       // b .
        fixture.cpu.lr = kVerifyCutEnd;
        fixture.cpu.attach_recompiled(&code);
        fixture.cpu.set_verify_recompiled(true);
        fixture.cpu.set_entry(entry);
    };

    g_mid_wrong = false;
    {
        Fixture fixture;
        prepare(fixture, kVerifyMidF + 4u);
        fixture.run(1u);
        const Cpu::VerifyReport& report = fixture.cpu.verify_report();
        require(report.verified == 1u && report.verified_instructions == 4u &&
                    report.mismatches == 0u,
                "a dispatch into the middle of a translated function must be verified");
        require(fixture.cpu.dispatch_entered_mid_function == 1u,
                "the verified mid-function dispatch is still counted as one");
        require(fixture.cpu.gpr[3] == 14u && fixture.cpu.pc == kVerifyCutEnd,
                "the verified mid-function route keeps the program's results");
    }
    g_mid_wrong = true;
    {
        Fixture fixture;
        prepare(fixture, kVerifyMidF + 4u);
        fixture.run(1u);
        require(fixture.cpu.verify_report().mismatches == 1u,
                "a wrong cold entry must be caught by the oracle");
    }
    g_mid_wrong = false;
    {
        Fixture fixture;
        prepare(fixture, kVerifyMidF + 8u);
        const u64 steps = fixture.run(1u);
        require(steps == 1u && fixture.cpu.gpr[3] == 4u && !fixture.cpu.stopped(),
                "a verified range entry that stalls at its own address must fall back to one "
                "interpreter step instead of re-dispatching until the bound is gone");
        require(fixture.cpu.verify_report().verified + fixture.cpu.verify_report().mismatches <= 1u,
                "the stalled entry must be attempted once, not spun on");
    }
}

// oracle-reference-7: the verify loop's host-page and diagnostic overheads.
void verify_register_only(Cpu& c) {
    if (c.try_hle(kVerifyCutA)) return;
    c.gpr[3] += 1u;
    c.pc = c.lr & ~3u;
}

void test_verify_recomp_segment_overheads() {
    Fixture fixture;
    fixture.bus.write32(kVerifyCutA, 0x38630001u);         // addi r3,r3,1
    fixture.bus.write32((kVerifyCutA + 4u), 0x4E800020u);  // blr
    fixture.bus.write32(kVerifyCutEnd, 0x48000000u);       // b .
    fixture.cpu.lr = kVerifyCutEnd;
    const RecompiledCode::Entry entries[] = {
        {kVerifyCutA, 8u, verify_register_only, "verify_register_only"}};
    RecompiledCode code;
    code.install(entries, 1u);
    fixture.cpu.attach_recompiled(&code);
    fixture.cpu.set_verify_recompiled(true);
    fixture.cpu.set_entry(kVerifyCutA);

    HostPageStats& stats = host_page_stats();
    const u64 invalidations_before = stats.invalidations;
    const u64 invalidation_ns_before = stats.invalidation_ns;
    const u64 replay_before = cpu_diag().verify_replay_executions;
    u64 cause5_before = 0;
    for (const auto& origin : cpu_diag().non_native_origins) {
        if (origin.cause == 5u) cause5_before += origin.count;
    }
    fixture.run(1u);
    require(fixture.cpu.verify_report().verified == 1u, "the register-only segment verifies");
    // journal_begin, restore_state, the replay's journal_begin, journal_end.
    require(stats.invalidations == invalidations_before + 4u,
            "every invalidation request is still counted");
    require(stats.invalidation_ns == invalidation_ns_before,
            "a verified segment must not memset host page tables that are provably empty");
    require(cpu_diag().verify_replay_executions == replay_before + 2u,
            "the replayed instructions (addi, blr) are counted in the scalar replay ledger");
    u64 cause5_after = 0;
    for (const auto& origin : cpu_diag().non_native_origins) {
        if (origin.cause == 5u) cause5_after += origin.count;
    }
    require(cause5_after == cause5_before,
            "replayed instructions must not occupy interpreter-origin slots");
    // (The memoised-page remap check that followed is gone with M1: there are
    // no memoised host pages - the page table is the translation - and no BATs.)
}

// The native interrupt line is derived by NativeInterrupts from its sources;
// it is not architected state. The oracle's rollback before a replay
// (restore_state) used to drop it: a raised, unmasked ARAM source was never
// delivered again and the ARQ waiter hung (stage1/verify-5).
void test_verify_recomp_keeps_the_native_interrupt_line() {
    Fixture fixture;
    fixture.bus.write32(kVerifyCutA, 0x38630001u);         // addi r3,r3,1
    fixture.bus.write32((kVerifyCutA + 4u), 0x4E800020u);  // blr
    fixture.bus.write32(kVerifyCutEnd, 0x48000000u);       // b .
    fixture.cpu.lr = kVerifyCutEnd;
    const RecompiledCode::Entry entries[] = {
        {kVerifyCutA, 8u, verify_register_only, "verify_register_only"}};
    RecompiledCode code;
    code.install(entries, 1u);
    fixture.cpu.attach_recompiled(&code);
    fixture.cpu.set_verify_recompiled(true);
    fixture.cpu.set_msr(fixture.cpu.msr() & (~kMsrEE));  // pending, not deliverable
    fixture.cpu.set_native_interrupt(true);
    fixture.cpu.set_entry(kVerifyCutA);
    fixture.run(1u);
    require(fixture.cpu.verify_report().verified == 1u && fixture.cpu.gpr[3] == 1u,
            "the register-only segment verifies");
    require(fixture.cpu.native_interrupt(),
            "the verified segment's rollback must keep the native interrupt line");
}

void test_string_load_register_extent() {
    // load-store-2. lswi/lswx alter exactly CEIL(n,4) registers from rD
    // (wrapping r31 -> r0), and lswi's count is NB (0 means 32) whatever rD is.
    constexpr u32 kData = 0x80005000u;
    const auto lswi = [](u32 d, u32 a, u32 nb) {
        return (31u << 26) | (d << 21) | (a << 16) | (nb << 11) | (597u << 1);
    };
    const auto lswx = [](u32 d, u32 a, u32 b) {
        return (31u << 26) | (d << 21) | (a << 16) | (b << 11) | (533u << 1);
    };
    struct Case {
        const char* name;
        u32 word;
        u32 xer;
        std::vector<std::pair<u32, u32>> expect;  // register, value
    };
    const std::vector<Case> cases = {
        {"lswi r0,r3,4 loads 4 bytes, not 32", lswi(0, 3, 4), 0u,
         {{0, 0x11223344u}, {1, 0xA0000001u}, {8, 0xA0000008u}, {3, kData}}},
        {"lswi r3,r4,4 leaves the base register", lswi(3, 4, 4), 0u,
         {{3, 0x11223344u}, {4, kData}, {5, 0xA0000005u}}},
        {"lswi r5,r4,8 leaves r7", lswi(5, 4, 8), 0u,
         {{5, 0x11223344u}, {6, 0x55667788u}, {7, 0xA0000007u}}},
        {"lswi r5,r4,6 partial word", lswi(5, 4, 6), 0u,
         {{5, 0x11223344u}, {6, 0x55660000u}, {7, 0xA0000007u}}},
        {"lswi r30,r4,12 wraps to r0", lswi(30, 4, 12), 0u,
         {{30, 0x11223344u}, {31, 0x55667788u}, {0, 0x99AABBCCu}, {1, 0xA0000001u}}},
        {"lswi r5,r4,0 loads 32 bytes", lswi(5, 4, 0), 0u,
         {{5, 0x11223344u}, {12, 0x0D0E0F10u}, {13, 0xA000000Du}}},
        {"lswx r5,r4,r0 XER count 4", lswx(5, 4, 0), 4u,
         {{5, 0x11223344u}, {6, 0xA0000006u}}},
    };
    for (const Case& test : cases) {
        Fixture fixture;
        const u8 bytes[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                            0x99, 0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x03, 0x04,
                            0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
                            0x01, 0x02, 0x03, 0x04, 0x0D, 0x0E, 0x0F, 0x10,
                            0xEE, 0xEE, 0xEE, 0xEE};
        for (u32 index = 0; index < sizeof(bytes); ++index) {
            fixture.bus.write8((kData) + index, bytes[index]);
        }
        for (u32 reg = 0; reg < 32u; ++reg) fixture.cpu.gpr[reg] = 0xA0000000u | reg;
        fixture.cpu.gpr[3] = kData;
        fixture.cpu.gpr[4] = kData;
        if (((test.word >> 1) & 0x3ffu) == 533u) fixture.cpu.gpr[0] = 0u;  // rB = r0 as the index
        fixture.cpu.xer = test.xer;
        fixture.emit({test.word});
        fixture.cpu.step();
        require(!fixture.cpu.stopped(), std::string(test.name) + ": the instruction must execute");
        for (const auto& [reg, value] : test.expect) {
            if (fixture.cpu.gpr[reg] != value) {
                char row[192];
                std::snprintf(row, sizeof(row), "%s: r%u = 0x%08X, expected 0x%08X", test.name,
                              reg, fixture.cpu.gpr[reg], value);
                require(false, row);
            }
        }
    }
}

void test_unsigned_divide_intrinsic_matches_recovered_helper() {
    // The retail DOL's complete 0x803A1F54 compiler helper, relocated as a
    // self-contained oracle. All of its branches are relative.
    constexpr u32 words[] = {
        0x2C030000u, 0x7C600034u, 0x7C890034u, 0x40820008u, 0x38090020u,
        0x2C050000u, 0x7CA90034u, 0x7CCA0034u, 0x40820008u, 0x392A0020u,
        0x7C004800u, 0x21400040u, 0x418100B0u, 0x39290001u, 0x21290040u,
        0x7C004A14u, 0x7D295050u, 0x7D2903A6u, 0x2C090020u, 0x38E9FFE0u,
        0x41800010u, 0x7C683C30u, 0x38E00000u, 0x48000018u, 0x7C884C30u,
        0x20E90020u, 0x7C673830u, 0x7D083B78u, 0x7C674C30u, 0x2C000020u,
        0x3120FFE0u, 0x41800010u, 0x7C834830u, 0x38800000u, 0x48000018u,
        0x7C630030u, 0x21200020u, 0x7C894C30u, 0x7C634B78u, 0x7C840030u,
        0x3940FFFFu, 0x30E70000u, 0x7C842114u, 0x7C631914u, 0x7D084114u,
        0x7CE73914u, 0x7C064010u, 0x7D253911u, 0x41800010u, 0x7C080378u,
        0x7D274B78u, 0x300A0001u, 0x4200FFD8u, 0x7C842114u, 0x7C631914u,
        0x4E800020u, 0x38800000u, 0x38600000u, 0x4E800020u,
    };
    constexpr u64 cases[][2] = {
        {0u, 1u},
        {1u, 2u},
        {2u, 1u},
        {0xffffffffull, 3u},
        {0x100000000ull, 0xffffffffull},
        {0x7fffffffffffffffull, 0x100000001ull},
        {0x8000000000000000ull, 1u},
        {0xfedcba9876543210ull, 0x123456789ull},
        {0xffffffffffffffffull, 0xffffffffffffffffull},
        {0xffffffffffffffffull, 1u},
    };

    for (const auto& values : cases) {
        Fixture oracle;
        u32 address = Fixture::kBase;
        for (const u32 word : words) {
            oracle.bus.write32(address, word);
            address += 4u;
        }
        constexpr u32 kReturn = 0x80004000u;
        oracle.cpu.lr = kReturn;
        oracle.cpu.cr = 0x13579bdfu;
        oracle.cpu.xer = kXerSO | kXerOV | kXerCA;
        oracle.cpu.ctr = 0x2468ace0u;
        for (u32 reg = 0; reg < 32u; ++reg) oracle.cpu.gpr[reg] = 0x11111111u * reg;
        oracle.cpu.gpr[3] = static_cast<u32>(values[0] >> 32u);
        oracle.cpu.gpr[4] = static_cast<u32>(values[0]);
        oracle.cpu.gpr[5] = static_cast<u32>(values[1] >> 32u);
        oracle.cpu.gpr[6] = static_cast<u32>(values[1]);
        for (u32 step = 0; oracle.cpu.pc != kReturn && step < 1024u; ++step) {
            oracle.cpu.step();
        }
        require(oracle.cpu.pc == kReturn, "recovered unsigned divide oracle must return");

        System native_system;
        Cpu& native = native_system.cpu();
        native.reset();
        native_system.start();
        native.lr = kReturn;
        native.cr = 0x13579bdfu;
        native.xer = kXerSO | kXerOV | kXerCA;
        native.ctr = 0x2468ace0u;
        for (u32 reg = 0; reg < 32u; ++reg) native.gpr[reg] = 0x11111111u * reg;
        native.gpr[3] = static_cast<u32>(values[0] >> 32u);
        native.gpr[4] = static_cast<u32>(values[0]);
        native.gpr[5] = static_cast<u32>(values[1] >> 32u);
        native.gpr[6] = static_cast<u32>(values[1]);
        require(native.try_hle(0x803A1F54u), "native unsigned divide intrinsic must handle nonzero divisors");

        for (u32 reg = 0; reg < 32u; ++reg) {
            require(native.gpr[reg] == oracle.cpu.gpr[reg],
                    "native unsigned divide intrinsic must preserve recovered GPR effects");
        }
        require(native.cr == oracle.cpu.cr, "native unsigned divide intrinsic must preserve CR");
        require(native.xer == oracle.cpu.xer, "native unsigned divide intrinsic must preserve XER");
        require(native.ctr == oracle.cpu.ctr, "native unsigned divide intrinsic must preserve CTR");
    }
}

// The retail DOL's signed 64-bit divide helper at 0x803A2040 (78 words, all
// branches relative), used as the interpreter oracle for the native intrinsic.
constexpr u32 kSignedDivideHelperWords[] = {
    0x9421FFF0u, 0x54690001u, 0x4182000Cu, 0x20840000u, 0x7C630190u, 0x91210008u,
    0x54AA0001u, 0x4182000Cu, 0x20C60000u, 0x7CA50190u, 0x9141000Cu, 0x2C030000u,
    0x7C600034u, 0x7C890034u, 0x40820008u, 0x38090020u, 0x2C050000u, 0x7CA90034u,
    0x7CCA0034u, 0x40820008u, 0x392A0020u, 0x7C004800u, 0x21400040u, 0x418100CCu,
    0x39290001u, 0x21290040u, 0x7C004A14u, 0x7D295050u, 0x7D2903A6u, 0x2C090020u,
    0x38E9FFE0u, 0x41800010u, 0x7C683C30u, 0x38E00000u, 0x48000018u, 0x7C884C30u,
    0x20E90020u, 0x7C673830u, 0x7D083B78u, 0x7C674C30u, 0x2C000020u, 0x3120FFE0u,
    0x41800010u, 0x7C834830u, 0x38800000u, 0x48000018u, 0x7C630030u, 0x21200020u,
    0x7C894C30u, 0x7C634B78u, 0x7C840030u, 0x3940FFFFu, 0x30E70000u, 0x7C842114u,
    0x7C631914u, 0x7D084114u, 0x7CE73914u, 0x7C064010u, 0x7D253911u, 0x41800010u,
    0x7C080378u, 0x7D274B78u, 0x300A0001u, 0x4200FFD8u, 0x7C842114u, 0x7C631914u,
    0x81210008u, 0x8141000Cu, 0x7D275279u, 0x4182001Cu, 0x2C090000u, 0x20840000u,
    0x7C630190u, 0x4800000Cu, 0x38800000u, 0x38600000u, 0x38210010u, 0x4E800020u,
};

// The retail DOL's unsigned 64-bit divide helper at 0x803A1F54 (59 words).
constexpr u32 kUnsignedDivideHelperWords[] = {
    0x2C030000u, 0x7C600034u, 0x7C890034u, 0x40820008u, 0x38090020u, 0x2C050000u,
    0x7CA90034u, 0x7CCA0034u, 0x40820008u, 0x392A0020u, 0x7C004800u, 0x21400040u,
    0x418100B0u, 0x39290001u, 0x21290040u, 0x7C004A14u, 0x7D295050u, 0x7D2903A6u,
    0x2C090020u, 0x38E9FFE0u, 0x41800010u, 0x7C683C30u, 0x38E00000u, 0x48000018u,
    0x7C884C30u, 0x20E90020u, 0x7C673830u, 0x7D083B78u, 0x7C674C30u, 0x2C000020u,
    0x3120FFE0u, 0x41800010u, 0x7C834830u, 0x38800000u, 0x48000018u, 0x7C630030u,
    0x21200020u, 0x7C894C30u, 0x7C634B78u, 0x7C840030u, 0x3940FFFFu, 0x30E70000u,
    0x7C842114u, 0x7C631914u, 0x7D084114u, 0x7CE73914u, 0x7C064010u, 0x7D253911u,
    0x41800010u, 0x7C080378u, 0x7D274B78u, 0x300A0001u, 0x4200FFD8u, 0x7C842114u,
    0x7C631914u, 0x4E800020u, 0x38800000u, 0x38600000u, 0x4E800020u,
};

// Operand pairs for the divide sweeps: every edge family the loop's control
// flow and its signed subtract test distinguish (bit lengths 0..64 on both
// sides, quotient 0/1/max, powers of two and their neighbours, divisors at and
// above 2^62 where the recovered loop stops being a true division, the signed
// extremes), plus deterministic random pairs at random bit lengths.
std::vector<std::pair<u64, u64>> divide_sweep_operands() {
    std::vector<u64> edges = {0u, 1u, 2u, 3u, 7u, 10u, 60u, 1000u, 125000u, 162000000u,
                              0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x100000000ull,
                              0x100000001ull, 0x123456789ABCull};
    for (u32 bit = 0; bit < 64u; ++bit) {
        const u64 power = u64{1} << bit;
        edges.push_back(power);
        edges.push_back(power - 1u);
        edges.push_back(~power + 1u);  // -power
    }
    std::vector<std::pair<u64, u64>> pairs;
    for (const u64 dividend : edges) {
        for (const u64 divisor : edges) {
            if (divisor != 0u) pairs.emplace_back(dividend, divisor);
        }
    }
    u64 seed = 0x9E3779B97F4A7C15ull;
    const auto next = [&seed]() {
        u64 z = (seed += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30u)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27u)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31u);
    };
    for (u32 index = 0; index < 20000u; ++index) {
        const u32 dividend_bits = static_cast<u32>(next() % 65u);
        const u32 divisor_bits = 1u + static_cast<u32>(next() % 64u);
        const u64 dividend = dividend_bits == 0u ? 0u : next() >> (64u - dividend_bits);
        u64 divisor = next() >> (64u - divisor_bits);
        if (divisor == 0u) divisor = 1u;
        pairs.emplace_back(dividend, divisor);
    }
    return pairs;
}

// Runs one helper both ways - the retail words on the interpreter and the
// installed native hook - from identical state, and requires identical GPRs,
// CR, XER, CTR, pc and stack words.
void check_divide_intrinsic_against_recovered_helper(const char* name, u32 hook_address,
                                                     const u32* words, std::size_t word_count) {
    static constexpr u32 kReturn = 0x80004000u;
    static constexpr u32 kStack = 0x80F10000u;
    Fixture oracle;
    for (std::size_t index = 0; index < word_count; ++index) {
        oracle.bus.write32((Fixture::kBase) + static_cast<u32>(index) * 4u,
                           words[index]);
    }
    System native_system;
    native_system.start();
    Cpu& native = native_system.cpu();

    const auto seed = [](Cpu& cpu, u64 dividend, u64 divisor, u32 variant) {
        cpu.lr = kReturn;
        cpu.cr = 0x13579bdfu ^ (variant * 0x11111111u);
        cpu.xer = (variant & 1u) != 0u ? (kXerSO | kXerOV | kXerCA) : 0u;
        cpu.ctr = 0x2468ace0u;
        for (u32 reg = 0; reg < 32u; ++reg) cpu.gpr[reg] = 0x11111111u * reg + variant;
        cpu.gpr[1] = kStack;
        cpu.gpr[3] = static_cast<u32>(dividend >> 32u);
        cpu.gpr[4] = static_cast<u32>(dividend);
        cpu.gpr[5] = static_cast<u32>(divisor >> 32u);
        cpu.gpr[6] = static_cast<u32>(divisor);
        for (u32 offset = 0; offset < 16u; offset += 4u) {
            cpu.mem_write32(kStack - 16u + offset, 0xA5A5A5A5u ^ offset);
        }
    };

    u64 compared = 0;
    int divergences = 0;
    u32 variant = 0;
    for (const auto& [dividend, divisor] : divide_sweep_operands()) {
        ++variant;
        oracle.cpu.reset();
        seed(oracle.cpu, dividend, divisor, variant);
        oracle.cpu.set_entry(Fixture::kBase);
        for (u32 step = 0; oracle.cpu.pc != kReturn && step < 4096u; ++step) {
            oracle.cpu.step();
        }
        require(oracle.cpu.pc == kReturn, std::string(name) + ": recovered helper oracle must return");

        native.reset();
        seed(native, dividend, divisor, variant);
        require(native.try_hle(hook_address),
                std::string(name) + ": the intrinsic must serve a nonzero divide");

        bool state_matches = native.pc == oracle.cpu.pc && native.cr == oracle.cpu.cr &&
                             native.xer == oracle.cpu.xer && native.ctr == oracle.cpu.ctr;
        for (u32 reg = 0; state_matches && reg < 32u; ++reg) {
            state_matches = native.gpr[reg] == oracle.cpu.gpr[reg];
        }
        for (u32 offset = 0; state_matches && offset < 16u; offset += 4u) {
            state_matches = native.mem_read32(kStack - 16u + offset) ==
                            oracle.cpu.mem_read32(kStack - 16u + offset);
        }
        ++compared;
        if (!state_matches && ++divergences <= 16) {
            std::cout << "  " << name << " divergence: dividend 0x" << std::hex << dividend
                      << " divisor 0x" << divisor << std::dec;
            for (u32 reg = 0; reg < 32u; ++reg) {
                if (native.gpr[reg] != oracle.cpu.gpr[reg]) {
                    std::cout << " r" << reg << " 0x" << std::hex << native.gpr[reg] << "/0x"
                              << oracle.cpu.gpr[reg] << std::dec;
                }
            }
            if (native.cr != oracle.cpu.cr) std::cout << " CR";
            if (native.xer != oracle.cpu.xer) std::cout << " XER";
            if (native.ctr != oracle.cpu.ctr) std::cout << " CTR";
            std::cout << "\n";
        }
    }
    std::cout << "  " << name << ": " << compared << " operand pairs compared, " << divergences
              << " divergences\n";
    require(divergences == 0, std::string(name) + ": the native 64-bit divide must leave exactly "
                                                  "the recovered helper's state");
}

void test_signed_divide_intrinsic_matches_recovered_helper() {
    check_divide_intrinsic_against_recovered_helper(
        "signed 0x803A2040", 0x803A2040u, kSignedDivideHelperWords,
        sizeof(kSignedDivideHelperWords) / sizeof(kSignedDivideHelperWords[0]));
}

void test_unsigned_divide_intrinsic_matches_recovered_helper_full_state() {
    check_divide_intrinsic_against_recovered_helper(
        "unsigned 0x803A1F54", 0x803A1F54u, kUnsignedDivideHelperWords,
        sizeof(kUnsignedDivideHelperWords) / sizeof(kUnsignedDivideHelperWords[0]));
}

void test_divide_intrinsics_serve_every_nonzero_divide() {
    // Guest code runs in zero game time, so nothing can happen inside a call:
    // the intrinsic serves every nonzero divide whatever is pending - a
    // decrementer edge armed for the next tick, a stop request (taken at the
    // next recognition point) - and only a zero divisor reaches the recovered
    // body, with the machine untouched.
    static constexpr u64 kDividend = 0x0123456789ABCDEFull;
    static constexpr u64 kDivisor = 1000u;
    for (const u32 address : {0x803A2040u, 0x803A1F54u}) {
        const auto prepare = [](System& system, u64 divisor) -> Cpu& {
            system.start();
            Cpu& cpu = system.cpu();
            cpu.reset();
            cpu.lr = 0x80004000u;
            cpu.gpr[1] = 0x80F10000u;
            cpu.gpr[3] = static_cast<u32>(kDividend >> 32u);
            cpu.gpr[4] = static_cast<u32>(kDividend);
            cpu.gpr[5] = static_cast<u32>(divisor >> 32u);
            cpu.gpr[6] = static_cast<u32>(divisor);
            return cpu;
        };
        const auto require_served = [address](Cpu& cpu, const char* why) {
            require(cpu.try_hle(address), std::string("divide intrinsic must serve: ") + why);
            require(cpu.gpr[4] == static_cast<u32>(kDividend / kDivisor) &&
                        cpu.gpr[3] == static_cast<u32>((kDividend / kDivisor) >> 32u),
                    std::string("the served divide must leave the quotient: ") + why);
        };
        {
            System system;
            require_served(prepare(system, kDivisor), "nothing pending");
        }
        {
            System system;
            Cpu& cpu = prepare(system, kDivisor);
            cpu.set_decrementer(0u);  // the edge one tick away
            require_served(cpu, "a decrementer edge armed for the next tick");
        }
        {
            System system;
            Cpu& cpu = prepare(system, kDivisor);
            cpu.request_stop(StopReason::HostRequest, "divide test");
            require_served(cpu, "a stop request");
        }
        {
            System system;
            Cpu& cpu = prepare(system, 0u);
            const u32 r3 = cpu.gpr[3];
            require(!cpu.try_hle(address), "a zero divisor must reach the recovered body");
            require(cpu.gpr[3] == r3 && cpu.pc == 0u,
                    "a declined divide must leave the machine untouched");
        }
        {
            System plain;
            plain.set_divide_intrinsics(false);
            Cpu& cpu = prepare(plain, kDivisor);
            require(!cpu.try_hle(address), "--no-divide-intrinsics must not install the hook");
        }
    }
}

void test_hle_serves_identically_in_both_engines() {
    // step() at an HLE-hooked pc and a generated thunk's try_hle must serve the
    // call identically, and step() must resume at the link register as the
    // thunk path does. (Their retirement parity went with the instruction
    // clock: nothing counts instructions.)
    constexpr u64 kDividend = 123456789ull;
    constexpr u64 kDivisor = 10ull;
    constexpr u32 kReturn = 0x80004000u;

    const auto seed = [](Cpu& cpu) {
        cpu.reset();
        cpu.lr = kReturn;
        for (u32 reg = 0; reg < 32u; ++reg) cpu.gpr[reg] = 0x11111111u * reg;
        cpu.gpr[3] = static_cast<u32>(kDividend >> 32u);
        cpu.gpr[4] = static_cast<u32>(kDividend);
        cpu.gpr[5] = static_cast<u32>(kDivisor >> 32u);
        cpu.gpr[6] = static_cast<u32>(kDivisor);
    };

    System thunk_system;
    thunk_system.start();
    Cpu& thunk = thunk_system.cpu();
    seed(thunk);
    require(thunk.try_hle(0x803A1F54u), "thunk path must serve the divide");

    System step_system;
    step_system.start();
    Cpu& interp = step_system.cpu();
    seed(interp);
    interp.set_entry(0x803A1F54u);
    interp.step();

    require(interp.pc == kReturn, "interpreter HLE must resume at the link register");
    for (u32 reg = 0; reg < 32u; ++reg) {
        require(interp.gpr[reg] == thunk.gpr[reg], "both engines must leave the same registers");
    }
    require(interp.cr == thunk.cr && interp.xer == thunk.xer && interp.ctr == thunk.ctr,
            "both engines must leave the same CR, XER and CTR");
}

// The frC probe (Cpu::note_single_multiply_frc): a single-precision multiply whose frC is not a single counts as
// wide; of those, one whose bits below the 24th fraction bit are non-zero counts as changed by a 25-bit significand
// rounding. 1 + 2^-24 is wide (24 fraction bits) yet survives that rounding; 0.1 is wide and does not; 0.5 is a single.
void test_frc_probe_classifies_single_multiply_operands() {
    const u32 fmuls_f3_f1_f2 = (59u << 26) | (3u << 21) | (1u << 16) | (2u << 6) | (25u << 1);
    const u32 fmadds_f3_f1_f2_f4 = (59u << 26) | (3u << 21) | (1u << 16) | (4u << 11) | (2u << 6) | (29u << 1);
    struct Case { double c; u32 word; u64 wide, changes; };
    const Case cases[] = {{0.5, fmuls_f3_f1_f2, 0u, 0u},
                          {0.1, fmuls_f3_f1_f2, 1u, 1u},
                          {1.0 + std::ldexp(1.0, -24), fmuls_f3_f1_f2, 1u, 0u},
                          {0.1, fmadds_f3_f1_f2_f4, 1u, 1u}};
    for (const Case& t : cases) {
        Fixture fixture;
        fixture.emit({t.word});
        fixture.cpu.set_msr(fixture.cpu.msr() | kMsrFP);
        fixture.cpu.set_frc_probe(true);
        fixture.cpu.fpr[1].bits[0] = double_to_bits(3.0);
        fixture.cpu.fpr[2].bits[0] = double_to_bits(t.c);
        fixture.cpu.fpr[4].bits[0] = double_to_bits(1.0);
        fixture.cpu.step();
        const auto& sites = fixture.cpu.frc_probe_sites();
        const auto it = sites.find(Fixture::kBase);
        const u64 wide = it == sites.end() ? 0u : it->second.wide;
        const u64 changes = it == sites.end() ? 0u : it->second.rounding_changes;
        require(wide == t.wide && changes == t.changes, "the frC probe must classify the operand");
    }
}

void test_exception_restores_user_fprs_on_rfi() {
    GuestMemory bus;
    Cpu cpu(bus);
    cpu.reset();
    // Asserts the host FPR shadow, which is default-off since 2026-09-05.
    cpu.set_interrupt_fpr_shadow(true);
    cpu.pc = 0x804349F8u;
    cpu.fpr[1].bits[0] = double_to_bits(-3.1415927410125732);
    cpu.fpr[2].bits[0] = double_to_bits(2.0);
    cpu.fpr[4].bits[0] = double_to_bits(3.1415927410125732);
    cpu.exception_entry(Exception::ExternalInterrupt);
    cpu.fpr[2].bits[0] = double_to_bits(0.0);
    cpu.exception_entry(Exception::SystemCall);
    cpu.restore_interrupt_fpr();
    require(bits_to_double(cpu.fpr[2].bits[0]) == 0.0,
            "inner rfi must not restore while the outer handler is still running");
    cpu.fpr[1].bits[0] = double_to_bits(-1.0);
    cpu.fpr[2].bits[0] = double_to_bits(0.0);
    cpu.fpr[4].bits[0] = double_to_bits(2147483648.0);
    cpu.restore_interrupt_fpr();
    require(bits_to_double(cpu.fpr[2].bits[0]) == 2.0,
            "outer rfi must give the interrupted code its FPR constants back");
    require(bits_to_double(cpu.fpr[1].bits[0]) < -3.0,
            "f1 must be -pi again, not the handler's scratch -1");
}

// Cpu::save_state writes the version-11 layout: the architected block, the
// interrupt FPR context block and the timeline block (the game clock, its five
// deadlines and the data-fault latch). Every older version is refused: version
// 9's timeline was an instruction count, which no longer exists, and version
// 10's had four deadlines (no retrace).

void test_cpu_savestate_preserves_nested_interrupt_fpr_context() {
    GuestMemory source_bus;
    Cpu source(source_bus);
    source.reset();
    // Asserts the host FPR shadow, which is default-off since 2026-09-05.
    source.set_interrupt_fpr_shadow(true);
    source.pc = 0x804349f8u;
    source.npc = source.pc + 4u;
    source.fpscr = 0x60004000u;
    source.fpr[1].bits[0] = double_to_bits(-3.1415927410125732);
    source.fpr[2].bits[0] = double_to_bits(2.0);
    source.fpr[4].bits[0] = double_to_bits(3.1415927410125732);
    source.exception_entry(Exception::ExternalInterrupt);
    source.fpr[1].bits[0] = double_to_bits(-1.0);
    source.fpr[2].bits[0] = double_to_bits(0.0);
    source.fpscr = 0u;
    source.exception_entry(Exception::SystemCall);

    std::vector<u8> blob;
    source.save_state(blob);

    GuestMemory restored_bus;
    Cpu restored(restored_bus);
    restored.reset();
    // The shadow is default-off since 2026-09-05; this test asserts its
    // behaviour, so both sides of the round trip opt in.
    restored.set_interrupt_fpr_shadow(true);
    restored.fpr[1].bits[0] = double_to_bits(99.0);
    restored.fpr[2].bits[0] = double_to_bits(99.0);
    const u8* cursor = blob.data();
    const u8* const end = blob.data() + blob.size();
    require(restored.load_state(cursor, end, 11u),
            "CPU state with a nested FPR context must load");
    require(cursor == end, "CPU state loader must consume the private FPR context block");

    restored.restore_interrupt_fpr();
    require(bits_to_double(restored.fpr[2].bits[0]) == 0.0,
            "inner rfi after a load must preserve the handler's current FPRs");
    restored.restore_interrupt_fpr();
    require(bits_to_double(restored.fpr[1].bits[0]) < -3.0 &&
                bits_to_double(restored.fpr[2].bits[0]) == 2.0 &&
                bits_to_double(restored.fpr[4].bits[0]) > 3.0,
            "outer rfi after a load must restore the interrupted thread's FPR file");
    require(restored.fpscr == 0x60004000u,
            "outer rfi after a load must restore the interrupted FPSCR");
}

// load-store-3: the lwarx reservation is architected state. Version 6 carries
// it inside the CPU block's former tail padding; the block size must not move,
// and an older version must ignore those bytes and load with no reservation.
// load-store-3: the lwarx reservation is architected state, carried inside the
// CPU block's former tail padding (the block size never moved).
void test_cpu_savestate_carries_reservation() {
    GuestMemory source_bus;
    Cpu source(source_bus);
    source.reset();
    source.gpr[3] = 0x12345678u;
    source.reservation = true;
    source.reservation_address = 0x80300200u;
    std::vector<u8> blob;
    source.save_state(blob);

    GuestMemory bus;
    Cpu current(bus);
    current.reset();
    const u8* cursor = blob.data();
    require(current.load_state(cursor, blob.data() + blob.size(), 11u),
            "a version-11 CPU state must load");
    require(cursor == blob.data() + blob.size(), "the loader must consume exactly the CPU blocks");
    require(current.reservation && current.reservation_address == 0x80300200u &&
                current.gpr[3] == 0x12345678u,
            "the savestate must restore the reservation");

    Cpu legacy(bus);
    legacy.reset();
    cursor = blob.data();
    require(!legacy.load_state(cursor, blob.data() + blob.size(), 9u),
            "a state from before native game time must be refused");
    cursor = blob.data();
    require(!legacy.load_state(cursor, blob.data() + blob.size(), 10u),
            "a state from before the native video (four deadlines) must be refused");

    Cpu reset_cpu(bus);
    reset_cpu.reservation = true;
    reset_cpu.reservation_address = 0x80300200u;
    reset_cpu.reset();
    require(!reset_cpu.reservation && reset_cpu.reservation_address == 0u,
            "hard reset must clear the reservation");
}

// ---- 2026-09-13 recompiler-core audit, batch b4 (state / memory / HLE) ------

// state-determinism-4: the hardware-service phase is part of the savestate.
// The writer retires 37 instructions, saves, and retires 63 more, ticking the
// devices once with 100 cycles at instruction 100. A fresh-process load and a
// hotkey load into a machine with its own leftover phase must tick at that
// same instruction with those same cycles. A pre-version-7 file loads with a
// zero phase however it is loaded (what a fresh process always gave it).
// Version 10: the timeline block is the game clock (now and every deadline)
// and, with the architected block, the time-base and decrementer views. A
// fresh-process load and a hotkey load into a machine with its own clock must
// both resume the writer's timeline exactly.
void test_cpu_savestate_carries_the_game_clock() {
    GuestMemory writer_bus;
    Cpu writer(writer_bus);
    writer.reset();
    writer.game_clock().set_now(123456789u);
    writer.game_clock().schedule(GameEvent::Frame, 123456789u + 1000u);
    writer.game_clock().schedule(GameEvent::Audio, 123456789u + 7000u);
    writer.set_time_base(0x1122334455667788ull);
    writer.set_decrementer(5000u);
    std::vector<u8> blob;
    writer.save_state(blob);

    const auto require_same_timeline = [&writer](const Cpu& cpu, const std::string& how) {
        require(cpu.game_clock().now() == writer.game_clock().now() &&
                    cpu.game_clock().deadlines() == writer.game_clock().deadlines(),
                how + ": the game clock and every deadline");
        require(cpu.time_base() == writer.time_base() && cpu.decrementer() == writer.decrementer(),
                how + ": the time-base and decrementer views");
    };

    GuestMemory fresh_bus;
    Cpu fresh(fresh_bus);
    fresh.reset();
    const u8* cursor = blob.data();
    require(fresh.load_state(cursor, blob.data() + blob.size(), 11u) &&
                cursor == blob.data() + blob.size(),
            "a version-11 CPU state loads and consumes the timeline block");
    require_same_timeline(fresh, "fresh-process load");

    GuestMemory hot_bus;
    Cpu hot(hot_bus);
    hot.reset();
    hot.game_clock().set_now(999u);
    hot.set_decrementer(1u);
    hot.game_clock().schedule(GameEvent::Devices, 5u);
    cursor = blob.data();
    require(hot.load_state(cursor, blob.data() + blob.size(), 11u), "hotkey load of version 11");
    require_same_timeline(hot, "hotkey load (its own clock and deadlines are replaced)");

    // The same game time later, the views still agree.
    for (Cpu* cpu : {&writer, &fresh, &hot}) {
        cpu->game_clock().set_now(cpu->game_clock().now() + 4000u);
    }
    require(fresh.decrementer() == writer.decrementer() && hot.time_base() == writer.time_base() &&
                writer.decrementer() == 1000u,
            "the loaded views must keep counting with the clock");
}

// Decrementer delivery (Cpu::set_decrementer_delivery): the native boot drops the sign edge because this program has no
// alarm; a guest mtdec arms the decrementer, so delivery comes back and the missing handler is named.
void test_decrementer_delivery_off_by_boot_on_by_mtdec() {
    // Default: the architecture. An edge is latched and pending.
    {
        Fixture f;
        f.emit({0x60000000u});
        f.cpu.set_msr(f.cpu.msr() | kMsrEE);
        require(f.cpu.decrementer_delivery(), "delivery is on by default");
        const u64 before = cpu_diag().dec_edge_dropped_no_consumer;
        f.cpu.latch_decrementer();
        require(f.cpu.decrementer_pending() && cpu_diag().dec_edge_dropped_no_consumer == before,
                "with delivery on, an edge is latched");
    }
    // Off: the edge is counted and dropped, nothing is pending, the next edge is still armed.
    {
        Fixture f;
        f.emit({0x60000000u});
        f.cpu.set_msr(f.cpu.msr() | kMsrEE);
        f.cpu.set_decrementer_delivery(false);
        const u64 total = cpu_diag().dec_edge_total, dropped = cpu_diag().dec_edge_dropped_no_consumer;
        const u64 deadline = f.cpu.game_clock().deadline(GameEvent::Decrementer);
        f.cpu.latch_decrementer();
        require(!f.cpu.decrementer_pending(), "with delivery off, no decrementer condition is pending");
        require(cpu_diag().dec_edge_total == total + 1u && cpu_diag().dec_edge_dropped_no_consumer == dropped + 1u,
                "the dropped edge is counted (total and dropped)");
        require(f.cpu.game_clock().deadline(GameEvent::Decrementer) == deadline + (u64{1} << 32),
                "the next edge is armed 2^32 ticks on, as for a delivered edge");
        // a guest mtdec r3 (spr 22) arms the decrementer: delivery is back
        f.bus.write32(Fixture::kBase, 0x7C7603A6u);  // mtspr DEC, r3
        f.cpu.gpr[3] = 1000u;
        f.cpu.set_entry(Fixture::kBase);
        f.cpu.step();
        require(f.cpu.decrementer_delivery() && f.cpu.decrementer() == 1000u,
                "a guest mtdec turns delivery back on and writes DEC");
    }
    // Delivery on and no handler installed (the OS's default one at exception 8): the stop names the reason.
    {
        constexpr u32 kDefaultHandler = 0x803716BCu;
        Fixture f;
        f.emit({0x60000000u});
        install_os_exception(f.bus, Exception::Decrementer, 8u, kDefaultHandler);
        f.cpu.set_msr(f.cpu.msr() | kMsrEE);
        f.cpu.latch_decrementer();
        f.cpu.run_steps(2u);  // the pending condition is taken at the first recognition point
        require(f.cpu.stopped(), "a decrementer exception without a handler stops the machine");
        require(f.cpu.stop().detail.find("no decrementer handler") != std::string::npos &&
                    f.cpu.stop().detail.find("mtdec") != std::string::npos,
                "the stop names the missing decrementer handler and the guest's mtdec: " + f.cpu.stop().detail);
    }
    // ...and with a handler installed the exception is entered, as before.
    {
        constexpr u32 kHandler = 0x80005000u;
        Fixture f;
        f.emit({0x60000000u});
        install_os_exception(f.bus, Exception::Decrementer, 8u, kHandler);
        f.bus.write32(kHandler, 0x48000000u);  // b . : the handler spins, so extra steps change nothing
        f.cpu.set_msr(f.cpu.msr() | kMsrEE);
        f.cpu.latch_decrementer();
        f.cpu.run_steps(1u);
        require(!f.cpu.stopped() && f.cpu.pc == kHandler && f.cpu.gpr[3] == 8u,
                "with a handler registered the decrementer exception enters it");
    }
}

// state-determinism-6: a Cpu reused through reset() must run exactly like a
// freshly constructed one.
// state-determinism-6: a Cpu reused through reset() must run exactly like a
// freshly constructed one.
void test_cpu_reset_matches_fresh_construction() {
    GuestMemory used_bus;
    Cpu used(used_bus);
    // Leave a moved clock, time-base and decrementer views, a latched edge, a
    // stop request and a pending hand-back behind.
    used.game_clock().set_now(5000u);
    used.set_time_base(77u);
    used.set_decrementer(0u);
    used.set_msr(used.msr() | (kMsrEE));
    used.latch_decrementer();
    used.request_stop(StopReason::HostRequest, "stop requested outside run()");
    used.leave_to_dispatcher(Fixture::kBase, FallbackReason::Yield, Fixture::kBase);
    used.reset();

    GuestMemory fresh_bus;
    Cpu fresh(fresh_bus);
    require(!used.exiting() && !used.stopped() && !used.needs_dispatch(),
            "reset must clear a pending hand-back, a stop and the stop request");
    require(used.game_clock().now() == fresh.game_clock().now() &&
                used.game_clock().deadlines() == fresh.game_clock().deadlines(),
            "reset must restart the game clock with nothing scheduled");
    require(used.time_base() == fresh.time_base() && used.decrementer() == fresh.decrementer(),
            "reset must restart the time-base and decrementer views");
    require(!used.decrementer_pending(), "reset must discard a latched decrementer condition");

    // A decrementer edge latched before the reset must not be taken after it.
    Fixture reused;
    reused.cpu.set_decrementer(0u);
    reused.cpu.set_msr(reused.cpu.msr() | (kMsrEE));
    reused.cpu.latch_decrementer();
    reused.cpu.reset();
    reused.emit({0x60000000u});  // nop
    reused.cpu.set_decrementer(0x7fffffffu);
    reused.cpu.set_msr(reused.cpu.msr() | (kMsrEE));
    reused.run(1);
    require(reused.cpu.spr[SPR_SRR0] == 0u && reused.cpu.pc == Fixture::kBase + 4u &&
                (reused.cpu.msr() & kMsrEE) != 0u,
            "reset must discard a latched decrementer exception");
}

// ---- 2026-09-13 recompiler-core audit, batch b5 (interrupt delivery) --------
//
// Recognition points R1-R5 are defined in cpu.h (interrupt_lines_). The OS
// exception entry is native (os_exception_entry.cpp): __OSExceptionTable names
// a handler per index whose first instruction marks it (`li r31,<vector>`), so
// a taken exception is visible as r31 after one interpreted instruction and as
// the OSContext's saved SRR0 / SRR1 (+0x198 / +0x19C).

constexpr u32 kIrqScFn = 0x80100300u;
constexpr u32 kIrqFpFn = 0x80100400u;
constexpr u32 kIrqHandlers = 0x80100800u;
constexpr u32 kIrqFpHandler = kIrqHandlers + 0x200u;

void install_irq_handlers(GuestMemory& memory) {
    // External (4) and decrementer (8): a marker, then a branch to itself.
    install_os_exception(memory, Exception::ExternalInterrupt, 4u, kIrqHandlers);
    memory.write32(kIrqHandlers, 0x3BE00500u);           // li r31,0x500
    memory.write32(kIrqHandlers + 4u, 0x48000000u);      // b .
    install_os_exception(memory, Exception::Decrementer, 8u, kIrqHandlers + 0x100u);
    memory.write32(kIrqHandlers + 0x100u, 0x3BE00900u);  // li r31,0x900
    memory.write32(kIrqHandlers + 0x104u, 0x48000000u);  // b .
    // FP unavailable (7): returns to the faulting instruction with MSR[FP], as
    // OSSwitchFPUContext does: SRR0 / SRR1 from the context, then rfi.
    install_os_exception(memory, Exception::FpUnavailable, 7u, kIrqFpHandler);
    const u32 fp_handler[] = {
        0x80640198u,  // lwz r3,0x198(r4)
        0x7C7A03A6u,  // mtsrr0 r3
        0x8064019Cu,  // lwz r3,0x19C(r4)
        0x60632000u,  // ori r3,r3,0x2000 (MSR[FP])
        0x7C7B03A6u,  // mtsrr1 r3
        0x4C000064u,  // rfi
    };
    for (u32 index = 0; index < 6u; ++index) memory.write32(kIrqFpHandler + 4u * index, fp_handler[index]);
    // System call (9): PPCSync's head, which the entry performs natively.
    install_os_exception(memory, Exception::SystemCall, 9u, 0u);
}

// Generated shape of `<12 instructions>; sc` whose flush crosses DEC 0 -> -1.
// Generated shape of `...; sc`: the system call hands back at the 0xC00
// vector, SRR0 at the following instruction.
void irq_sc_fn(Cpu& c) {
    if (c.try_hle(kIrqScFn)) return;
    recomp::system_call(c, kIrqScFn + 48u);
}

// Generated shape of `<12 instructions>; lfs` with MSR[FP] = 0: fp_unavailable
// retires the pending batch (crossing the edge) before raising 0x800.
// Generated shape of `...; lfs` with MSR[FP] = 0: FP-unavailable hands back
// at the 0x800 vector, SRR0 at the faulting instruction.
void irq_fp_fn(Cpu& c) {
    if (c.try_hle(kIrqFpFn)) return;
    if ((c.msr() & 0x2000u) == 0u) {
        recomp::fp_unavailable(c, kIrqFpFn + 48u);
        return;
    }
    c.leave_to_dispatcher(kIrqFpFn + 52u, FallbackReason::NotRecompiled, kIrqFpFn + 48u);
}

// dispatch-exceptions-1: a decrementer exception condition is delayed while
// MSR[EE] = 0, never discarded - at the edge itself (both engines), and at the
// dispatcher after an sc or FP-unavailable hand-back has cleared EE.
// gekko_user_manual.txt PAGE 164 Table 4-4 (EE = 0 "delays recognition"), PAGE
// 161 (the asynchronous exception "remains pending"), PAGE 475 (mtmsr setting
// EE takes it before the next instruction).
// dispatch-exceptions-1: a decrementer exception condition is delayed while
// MSR[EE] = 0, never discarded - by both engines, and at the dispatcher after
// an sc or FP-unavailable hand-back has cleared EE. gekko_user_manual.txt PAGE
// 164 Table 4-4 (EE = 0 "delays recognition"), PAGE 161 (the asynchronous
// exception "remains pending"), PAGE 475 (mtmsr setting EE takes it before the
// next instruction). Under native game time the edge is an event: the
// scheduler calls latch_decrementer at its deadline, at a wait point.
void test_decrementer_condition_is_delayed_not_discarded() {
    // 1. Latched with EE = 0, then mtmsr sets EE: 0x900 is taken right after
    //    the mtmsr, by the dispatcher and by the interpreter alike.
    for (bool through_run : {true, false}) {
        Fixture f;
        install_irq_handlers(f.bus);
        f.emit({0x7CA00124u /* mtmsr r5 */, 0x38600001u /* li r3,1 */});
        f.cpu.set_msr(f.cpu.msr() & (~kMsrEE));
        f.cpu.gpr[5] = f.cpu.msr() | kMsrEE;
        f.cpu.latch_decrementer();
        require(f.cpu.decrementer_pending() && !f.cpu.interrupt_pending(),
                "an edge delivered with EE = 0 is pending, not deliverable");
        if (through_run) {
            f.run(2u);
        } else {
            f.cpu.step();
            f.cpu.step();
        }
        const char* engine = through_run ? "dispatcher: " : "interpreter step: ";
        require(f.cpu.gpr[31] == 0x900u && f.bus.read32(kTestContext + 0x198u) == Fixture::kBase + 4u &&
                    (f.bus.read32(kTestContext + 0x19Cu) & kMsrEE) != 0u && !f.cpu.decrementer_pending(),
                std::string(engine) +
                    "an edge latched with EE = 0 must be delivered right after mtmsr sets EE");
    }

    // 2. With EE = 0 the latch requests no exit; setting EE makes it
    //    deliverable to every native poll.
    {
        Fixture f;
        f.cpu.set_msr(f.cpu.msr() & (~kMsrEE));
        f.cpu.latch_decrementer();
        require(f.cpu.decrementer_pending() && !f.cpu.interrupt_pending() &&
                    !f.cpu.needs_dispatch() && !f.cpu.exiting(),
                "an EE = 0 edge must latch without being deliverable");
        f.cpu.set_msr(f.cpu.msr() | (kMsrEE));
        require(f.cpu.interrupt_pending() && f.cpu.needs_dispatch(),
                "setting EE must make the latched condition deliverable");
    }

    // 3. The FP-unavailable hand-back clears EE. An edge the scheduler
    //    delivers while the OS handler runs with EE = 0 must survive, and be
    //    taken right after the handler's rfi restores EE, with the rfi's
    //    resume address as the interrupted SRR0. A hook on the handler
    //    delivers it (as a wait point inside it would) and declines, so the
    //    handler runs.
    {
        Fixture f;
        install_irq_handlers(f.bus);
        const RecompiledCode::Entry entries[] = {{kIrqFpFn, 52u, irq_fp_fn, "FP unavailable"}};
        RecompiledCode code;
        code.install(entries, 1u);
        f.cpu.attach_recompiled(&code);
        f.cpu.install_hle(kIrqFpHandler, [](Cpu& c) {
            c.latch_decrementer();
            return false;
        }, "edge delivered inside the handler");
        f.cpu.set_entry(kIrqFpFn);
        f.cpu.set_msr((f.cpu.msr() | kMsrEE) & ~kMsrFP);
        // The native body (1), the handler's six instructions (6), then the
        // decrementer through the OS entry to its marker (1).
        f.run(8u);
        require(!f.cpu.stopped() && f.cpu.gpr[31] == 0x900u &&
                    f.bus.read32(kTestContext + 0x198u) == kIrqFpFn + 48u && !f.cpu.decrementer_pending(),
                "FP unavailable: an edge latched while the handler runs with EE = 0 must be taken after "
                "its rfi (" + f.cpu.stop().detail + ")");
    }
    // The system call is PPCSync's head, performed natively and atomically by
    // the OS entry: no handler runs with EE = 0 inside it, and it returns with
    // the MSR it was entered with - an edge pending before it is taken right
    // after it, at the instruction that follows the sc.
    {
        Fixture f;
        install_irq_handlers(f.bus);
        const RecompiledCode::Entry entries[] = {{kIrqScFn, 52u, irq_sc_fn, "sc"}};
        RecompiledCode code;
        code.install(entries, 1u);
        f.cpu.attach_recompiled(&code);
        f.cpu.set_entry(kIrqScFn);
        f.cpu.set_msr(f.cpu.msr() | kMsrEE);
        const u32 msr = f.cpu.msr();
        f.run(1u);
        require(!f.cpu.stopped() && f.cpu.pc == kIrqScFn + 52u && f.cpu.msr() == msr &&
                    f.cpu.spr[SPR_SRR0] == kIrqScFn + 52u,
                "sc: the native entry returns to the next instruction with the MSR it was entered with (" +
                    f.cpu.stop().detail + ")");
        f.cpu.latch_decrementer();
        f.run(1u);
        require(f.cpu.gpr[31] == 0x900u && f.bus.read32(kTestContext + 0x198u) == kIrqScFn + 52u &&
                    !f.cpu.decrementer_pending(),
                "sc: an edge pending after the sc is taken at the instruction that follows it");
    }

    // 4. Savestate: the two on-disk bools are the two condition bits.
    {
        GuestMemory source_bus;
        Cpu source(source_bus);
        source.reset();
        source.set_external_interrupt(true);
        source.latch_decrementer();  // EE = 0 after reset: latched
        std::vector<u8> blob;
        source.save_state(blob);
        GuestMemory bus;
        Cpu restored(bus);
        restored.reset();
        const u8* cursor = blob.data();
        require(restored.load_state(cursor, blob.data() + blob.size(), 11u) &&
                    restored.external_interrupt() && restored.decrementer_pending(),
                "the savestate must carry both latched interrupt conditions");
        restored.set_external_interrupt(false);
        require(!restored.external_interrupt() && restored.decrementer_pending(),
                "dropping the external line must leave the decrementer condition pending");
    }
}

// dispatch-exceptions-2 / dispatch-exceptions-6: a condition that becomes
// deliverable while native code runs must be visible to every native poll
// without a new predicate term - needs_dispatch (back edges), interrupt_pending
// (mtmsr) and exiting (after every call).
void test_deliverable_interrupt_reaches_every_native_poll() {
    Fixture dec;
    dec.cpu.set_msr(dec.cpu.msr() | (kMsrEE));
    dec.cpu.set_decrementer(0u);
    require(!dec.cpu.needs_dispatch() && !dec.cpu.exiting(), "fixture starts idle");
    dec.cpu.latch_decrementer();  // the scheduler at the edge's deadline
    require(dec.cpu.decrementer_pending() && dec.cpu.interrupt_pending() &&
                dec.cpu.needs_dispatch() && dec.cpu.exiting(),
            "a decrementer edge with EE = 1 must reach needs_dispatch, interrupt_pending and exiting");

    Fixture ext;
    ext.cpu.set_msr(ext.cpu.msr() & (~kMsrEE));
    ext.cpu.set_external_interrupt(true);
    require(ext.cpu.external_interrupt() && !ext.cpu.needs_dispatch() && !ext.cpu.exiting(),
            "an external assertion with EE = 0 is pending, not deliverable");
    ext.cpu.set_msr(ext.cpu.msr() | (kMsrEE));
    ext.cpu.set_external_interrupt(true);  // e.g. an INTMR write re-evaluating the line
    require(ext.cpu.needs_dispatch() && ext.cpu.interrupt_pending() && ext.cpu.exiting(),
            "an external assertion with EE = 1 must request the call/return unwind");
}

// Generated shape of `addi r3,r3,1; blr` (the leaf of oracle-reference-5).
// `addi r3,r3,1; b -4` translated WITH its back-edge poll (good) or without it
// (missing a recognition point, 24 instructions).
// oracle-reference-5: DEC = 0, timer residue 11, EE = 1, `addi; blr`. The edge
// lands on the addi. The interpreter used to raise 0x900 there while native
// code only latched it, so the replay ended in the vector and the verified run
// stopped on a false mismatch. Both engines now take it at the same
// recognition point (here: after the blr, via the dispatcher). The oracle must
// still catch native code that runs past a recognition point.
// Under native game time an edge is delivered only at a wait point, so it
// can never fall inside a verified segment (a segment that reaches a wait
// point is not replayed). What remains to pin is the other half of the old
// test: a condition a wait point delivers inside a translated loop reaches
// that loop's back-edge poll, and the dispatcher takes it with the clock at
// the event.
constexpr u32 kWaitLoopFn = 0x80100500u;
constexpr u32 kWaitFlag = 0x80110040u;

// Generated shape of `lwz r0,flag ; cmplwi r0,0 ; beq -8`, a listed wait
// point (extract/wait-points.txt): the taken back edge waits, then polls.
void wait_loop_fn(Cpu& c) {
    if (c.try_hle(kWaitLoopFn)) return;
loop:
    c.gpr[0] = c.mem_read32(kWaitFlag);
    if (c.gpr[0] == 0u) {
        c.pc = kWaitLoopFn;
        c.idle_wait();
        if (c.needs_dispatch()) {
            c.leave_to_dispatcher(kWaitLoopFn, FallbackReason::Yield, kWaitLoopFn + 8u);
            return;
        }
        goto loop;
    }
    c.pc = c.lr & ~3u;
}

void test_wait_point_event_reaches_the_back_edge_poll() {
    System system;
    system.start();
    Cpu& cpu = system.cpu();
    install_irq_handlers(system.guest_memory());
    const RecompiledCode::Entry entries[] = {{kWaitLoopFn, 12u, wait_loop_fn, "wait_loop"}};
    RecompiledCode code;
    code.install(entries, 1u);
    cpu.attach_recompiled(&code);
    cpu.mem_write32(kWaitFlag, 0u);
    cpu.set_entry(kWaitLoopFn);
    cpu.set_msr(cpu.msr() | kMsrEE);
    const u64 start = system.game_ticks();
    // A System turns decrementer delivery off (no alarm in this program); the guest's mtdec turns it back on, as here.
    cpu.set_decrementer_delivery(true);
    cpu.set_decrementer(500u);  // the edge 501 ticks on, long before the first host service
    cpu.run_steps(2u);
    require(cpu.gpr[31] == 0x900u && system.guest_memory().read32(kTestContext + 0x198u) == kWaitLoopFn &&
                !cpu.decrementer_pending(),
            "the edge the wait delivered must be taken at the loop's back-edge poll");
    require(system.game_ticks() == start + 501u && system.idle_waits() == 1u,
            "the wait must stop the clock at the event that made the interrupt deliverable");
}

// memory-model-3: an access the window cannot serve halts at the FIRST fault
// (production's unmapped-access stops; a null pointer is the common case).
// Translated-code shape (entry thunk, straight-line accesses, a direct call
// and its exiting check):
//   lwz r3,0(r4)   r4 = 0   -> EA 0, nothing mapped: the fault
//   lwz r5,4(r3)            -> EA 4, a second miss that must not rewrite DAR
//   stw r5,8(r6)   r6 = 0x80110000, valid memory: must not commit
//   stb r7,0(r8)   r8 = 0xCC008000, a GX FIFO NOP: must not reach the pipe
//   bl callee               -> must not run the callee body
constexpr u32 kFaultFn = 0x80120000u;
constexpr u32 kFaultCallee = 0x80120100u;

void fault_callee(Cpu& c) {
    if (c.try_hle(kFaultCallee)) return;
    c.gpr[9] = 0xBADu;
    c.pc = c.lr & ~3u;
}

void fault_fn(Cpu& c) {
    if (c.try_hle(kFaultFn)) return;
    c.gpr[3] = c.mem_read32(c.gpr[4] + 0u);
    c.gpr[5] = c.mem_read32(c.gpr[3] + 4u);
    c.mem_write32(c.gpr[6] + 8u, c.gpr[5]);
    c.mem_write8(c.gpr[8], static_cast<u8>(c.gpr[7]));
    c.lr = kFaultFn + 20u;
    fault_callee(c);
    if (c.exiting() && c.finish_transfers()) return;
    c.gpr[10] = 0xBADu;
    c.leave_to_dispatcher(kFaultFn + 24u, FallbackReason::NotRecompiled, kFaultFn + 20u);
}

void test_unmapped_access_halts_at_first_fault() {
    Fixture fixture;
    fixture.cpu.set_unmapped_access_stops(true);
    RecordingGxSink sink;
    fixture.cpu.set_gx_sites(nullptr, 0u, &sink);  // an unlisted pipe store would stop
    const RecompiledCode::Entry entries[] = {
        {kFaultFn, 24u, fault_fn, "fault_fn"},
        {kFaultCallee, 8u, fault_callee, "fault_callee"},
    };
    RecompiledCode code;
    code.install(entries, 2u);
    fixture.cpu.attach_recompiled(&code);
    fixture.bus.write32(0x80110008u, 0x12345678u);
    fixture.cpu.gpr[4] = 0u;
    fixture.cpu.gpr[6] = 0x80110000u;
    fixture.cpu.gpr[7] = 0u;
    fixture.cpu.gpr[8] = 0xCC008000u;
    fixture.cpu.set_entry(kFaultFn);
    const u64 unmapped_before = fixture.bus.diagnostics().unmapped_writes;
    fixture.run(100);

    const Cpu& cpu = fixture.cpu;
    require(cpu.stop().reason == StopReason::ExecutionFault &&
                cpu.stop().detail.find("load from 0x00000000") != std::string::npos &&
                cpu.stop().detail.find("below the program image") != std::string::npos,
            "the stop must describe the first faulting access: " + cpu.stop().detail);
    require(cpu.spr[SPR_DAR] == 0u, "a later fault must not overwrite DAR");
    require(cpu.spr[SPR_DSISR] == kDsisrNoTranslation, "DSISR must describe a load with nothing mapped");
    require(cpu.memory_fault().latched && cpu.memory_fault().unmapped && cpu.memory_fault().effective == 0u &&
                !cpu.memory_fault().write && cpu.memory_fault().pc == kFaultFn,
            "the fault record must be the first fault's");
    require(fixture.bus.read32(0x80110008u) == 0x12345678u,
            "a store after the fault must not commit guest RAM");
    require(cpu.unlisted_gx_stores() == 0u && sink.calls.empty() &&
                fixture.bus.diagnostics().unmapped_writes == unmapped_before,
            "a pipe store after the fault must reach nothing");
    require(cpu.gpr[9] != 0xBADu, "a call after the fault must not run its callee");
    require(cpu.gpr[10] != 0xBADu, "the caller must unwind at the call boundary");

    // The fault is machine state: clearing the report does not resume the guest.
    const u32 pc_at_fault = fixture.cpu.pc;
    fixture.cpu.clear_stop();
    require(fixture.run(100) == 0u && fixture.cpu.pc == pc_at_fault &&
                fixture.cpu.stop().reason == StopReason::ExecutionFault &&
                fixture.cpu.stop().address == kFaultFn &&
                fixture.cpu.stop().detail.find("no memory of the game's address window") != std::string::npos,
            "a run after clear_stop must re-raise the latched fault without executing");

    // The interpreter halts on the same first fault with the same record.
    Fixture interpreted;
    interpreted.cpu.set_unmapped_access_stops(true);
    interpreted.emit({0x80640000u, 0x80A30004u, 0x90A60008u});  // lwz; lwz; stw
    interpreted.bus.write32(0x80110008u, 0x12345678u);
    interpreted.cpu.gpr[4] = 0u;
    interpreted.cpu.gpr[6] = 0x80110000u;
    interpreted.run(100);
    require(interpreted.cpu.stop().reason == StopReason::ExecutionFault &&
                interpreted.cpu.spr[SPR_DAR] == 0u &&
                interpreted.cpu.spr[SPR_DSISR] == kDsisrNoTranslation &&
                interpreted.cpu.memory_fault().pc == Fixture::kBase &&
                interpreted.bus.read32(0x80110008u) == 0x12345678u,
            "the interpreter must record the same first fault at the faulting instruction");

    // A store fault sets the DSISR store bit.
    Fixture store;
    store.cpu.set_unmapped_access_stops(true);
    store.cpu.mem_write16(0x00000010u, 0x1234u);
    require(store.cpu.spr[SPR_DAR] == 0x10u &&
                store.cpu.spr[SPR_DSISR] == (kDsisrNoTranslation | kDsisrStore) &&
                store.cpu.memory_fault().write,
            "a store fault must set DSISR[6]");

    // reset() replaces the machine and lifts the latch.
    fixture.cpu.reset();
    fixture.cpu.mem_write32(0x80110008u, 0xCAFEF00Du);
    require(!fixture.cpu.memory_fault().latched && !fixture.cpu.stopped() &&
                fixture.bus.read32(0x80110008u) == 0xCAFEF00Du,
            "reset must lift the fault latch");
}

// memory-model-4: host diagnostic peeks change nothing.
class CountingMmio : public MmioDevice {
public:
    const char* name() const override { return "counting"; }
    u8 read8(u32) override { ++reads; return 0xFF; }
    u16 read16(u32) override { ++reads; return 0xFFFF; }
    u32 read32(u32) override { ++reads; return 0xFFFFFFFFu; }
    u32 reads = 0;
};

void test_host_peeks_have_no_side_effects() {
    Fixture fixture;
    Cpu& cpu = fixture.cpu;
    // The last mapped bytes of the window: the OS heap region ends at
    // 0x81800000 (GuestMemory::kLegacyHeapEnd).
    cpu.spr[SPR_DAR] = 0xDEADBEEFu;
    for (u32 index = 0; index < 16u; ++index) {
        fixture.bus.write8(0x817FFFF0u + index, static_cast<u8>('A' + index));
    }
    u32 traced = 0;
    fixture.bus.set_access_trace([&](u32, std::size_t, bool) { ++traced; });
    CountingMmio mmio;
    fixture.bus.register_mmio(0xCC003000u, 0x100u, &mmio);
    const u64 unmapped_before = fixture.bus.diagnostics().unmapped_reads;

    require(cpu.read_guest_string(0x817FFFF0u, 128u) == "ABCDEFGHIJKLMNOP",
            "a string peek must stop at the end of the mapping");
    require(cpu.read_guest_u32(0x817FFFFEu) == 0u, "a peek straddling the mapping end reads 0");
    require(cpu.read_guest_u32(0x00000034u) == 0u, "an unmapped peek reads 0");
    require(cpu.read_guest_u8(0x817FFFF1u) == 'B', "a mapped byte peeks");
    require(cpu.read_guest_u32(0xCC003000u) == 0u && mmio.reads == 0u,
            "a peek must not perform a device read");
    u8 byte = 0x5A;
    require(!cpu.peek_guest_u8(0x81800000u, byte) && byte == 0x5A,
            "peek_guest_u8 reports an unmapped byte without touching the output");
    require(!cpu.stopped() && !cpu.memory_fault().latched && cpu.spr[SPR_DAR] == 0xDEADBEEFu,
            "a diagnostic peek must not fault, stop or clobber DAR");
    require(traced == 0u, "a diagnostic peek must not be traced as a guest access");
    require(fixture.bus.diagnostics().unmapped_reads == unmapped_before,
            "a diagnostic peek must not log an unmapped access");

    require(cpu.mem_read8(0x817FFFF0u) == 'A' && traced == 1u,
            "the guest's own access to the same byte is traced");
    fixture.bus.set_access_trace({});
}


// concurrency-4: every Cpu on a GuestMemory hears every translation change,
// and destroying one leaves the other's listener in place. Journalling is the
// change that must take every Cpu off the page table: a store served by a host
// pointer would bypass the journal and could not be rolled back.
void test_translation_listeners_are_per_cpu() {
    const auto journalled_store = [](GuestMemory& memory, Cpu& cpu, u32 value) {
        memory.journal_begin();
        cpu.mem_write32(0x80001000u, value);
        const bool recorded = memory.journal().size() == 1u;
        memory.journal_rollback();
        memory.journal_end();
        return recorded;
    };
    {
        GuestMemory bus;
        Cpu first(bus);
        first.mem_write32(0x80001000u, 0x11223344u);
        require(first.mem_read32(0x80001000u) == 0x11223344u, "first Cpu memoises the page");
        {
            Cpu second(bus);
            require(bus.translation_listener_count() == 2u, "both Cpus are registered");
            require(second.mem_read32(0x80001000u) == 0x11223344u, "second Cpu memoises the page");
        }
        require(bus.translation_listener_count() == 1u,
                "destroying the second Cpu must leave the first one's listener");
        require(journalled_store(bus, first, 0xAAAAAAAAu) && bus.read32(0x80001000u) == 0x11223344u,
                "journalling begun after the second Cpu died must still reach the first");
    }
    {
        GuestMemory bus;
        Cpu first(bus);
        Cpu second(bus);
        first.mem_write32(0x80001000u, 0x55667788u);
        require(first.mem_read32(0x80001000u) == 0x55667788u &&
                    second.mem_read32(0x80001000u) == 0x55667788u,
                "both Cpus memoise the page");
        require(journalled_store(bus, first, 0xBBBBBBBBu),
                "the earlier Cpu must hear journalling begin while a later one is alive");
        require(second.mem_read32(0x80001000u) == 0x55667788u, "both Cpus re-memoise the page");
        require(journalled_store(bus, second, 0xCCCCCCCCu) && bus.read32(0x80001000u) == 0x55667788u,
                "the later Cpu must hear the same change");
    }
}

// dispatch-exceptions-3: the try_hle filter rejects unhooked calls inline for
// any number of hooks and stays exact.
void test_try_hle_filter_is_exact_for_any_hook_count() {
    Fixture fixture;
    Cpu& cpu = fixture.cpu;
    // The eleven hooks System::start installs unconditionally, the AX mixer's
    // and the four compiler intrinsics: sixteen, twice the old hot list.
    const u32 hooks[] = {0x804086D8u, 0x80486718u, 0x801C28A8u, 0x802731A4u,
                         0x800BCBA4u, 0x800448C8u, 0x8036E794u, 0x8035F840u,
                         0x80434824u, 0x80449458u, 0x8035FD08u, 0x80386BC0u,
                         0x803A1F54u, 0x80051414u, 0x80051BBCu, 0x803A2040u};
    u32 served = 0;
    for (const u32 hook : hooks) {
        cpu.install_hle(hook, [&served](Cpu&) { ++served; return true; }, "counting hook");
    }
    u64 slow = cpu.hle_slow_calls();
    u32 unhooked = 0;
    for (u32 address = 0x80004000u; address < 0x80504000u; address += 4u) {
        if (cpu.has_hle(address)) continue;
        require(!cpu.try_hle(address), "an unhooked address must not be served");
        ++unhooked;
    }
    require(unhooked > 1000000u && cpu.hle_slow_calls() == slow,
            "no unhooked call may leave the inline filter (was: every call, 16 hooks > 8)");
    for (const u32 hook : hooks) require(cpu.try_hle(hook), "every hooked address must be served");
    require(served == 16u && cpu.hle_slow_calls() == slow + 16u, "each hooked call takes the slow path once");

    // Two hooks sharing a slot: both served, an unhooked address there still exact.
    cpu.install_hle(0x80100000u, [&served](Cpu&) { ++served; return true; }, "slot A");
    cpu.install_hle(0x80101000u, [&served](Cpu&) { ++served; return true; }, "slot B");
    require(cpu.try_hle(0x80100000u) && cpu.try_hle(0x80101000u) && served == 18u,
            "hooks sharing a slot are both served");
    require(!cpu.try_hle(0x80102000u), "an unhooked address in a shared slot is still declined");

    // A handler that declines runs the native body.
    cpu.install_hle(0x80200000u, [](Cpu&) { return false; }, "declines");
    require(!cpu.try_hle(0x80200000u), "a declining handler must not serve the call");

    // Verification sends every call to the slow path (the per-call cut).
    cpu.set_verify_recompiled(true);
    slow = cpu.hle_slow_calls();
    cpu.try_hle(0x80300000u);
    require(cpu.hle_slow_calls() == slow + 1u, "while verifying every call reaches try_hle_slow");
    cpu.set_verify_recompiled(false);
    slow = cpu.hle_slow_calls();
    require(!cpu.try_hle(0x80300004u) && cpu.hle_slow_calls() == slow,
            "switching verification off restores the inline reject");
}

// The condition register encodes less-than as 8, greater-than as 4 and equal
// as 2. Getting greater-than and equal the wrong way round makes every SDK
// spin-wait invert, which is how the L2 cache invalidate loop first hung.
void test_condition_register_encoding() {
    Fixture fixture;
    fixture.emit({
        0x38600005u,  // li r3, 5
        0x38800005u,  // li r4, 5
        0x7C032000u,  // cmpw cr0, r3, r4
    });
    fixture.run(3);
    require(fixture.cpu.cr_field(0) == 2u, "cmpw of equal values must set the equal bit");

    Fixture greater;
    greater.emit({
        0x38600009u,  // li r3, 9
        0x38800005u,  // li r4, 5
        0x7C032000u,  // cmpw cr0, r3, r4
    });
    greater.run(3);
    require(greater.cpu.cr_field(0) == 4u, "cmpw of a larger value must set the greater bit");

    Fixture less;
    less.emit({
        0x38600001u,  // li r3, 1
        0x38800005u,  // li r4, 5
        0x7C032000u,  // cmpw cr0, r3, r4
    });
    less.run(3);
    require(less.cpu.cr_field(0) == 8u, "cmpw of a smaller value must set the less bit");
}

// A system call resumes at the instruction after sc (resuming at sc itself
// makes the SDK cache-flush syscall loop forever). The OS's system-call entry is
// PPCSync's head, done natively: HID0[ABE] set and restored (r9 = HID0, r10 =
// HID0 | 8), then rfi to the following instruction with the entry's MSR.
void test_system_call_return_address() {
    Fixture fixture;
    install_os_exception(fixture.bus, Exception::SystemCall, 9u, 0u);
    fixture.emit({
        0x44000002u,  // sc
    });
    const u32 msr = fixture.cpu.msr();
    const u32 hid0 = fixture.cpu.spr[SPR_HID0];
    fixture.run(1);
    require(fixture.cpu.spr[SPR_SRR0] == Fixture::kBase + 4u,
            "sc must save the address of the following instruction");
    require(fixture.cpu.pc == Fixture::kBase + 4u && fixture.cpu.msr() == msr && !fixture.cpu.stopped(),
            "sc must return to the following instruction with the MSR it was entered with");
    require(fixture.cpu.gpr[9] == hid0 && fixture.cpu.gpr[10] == (hid0 | 8u) &&
                fixture.cpu.spr[SPR_HID0] == hid0,
            "sc's PPCSync head leaves r9 = HID0, r10 = HID0 | ABE and HID0 unchanged");
}

void test_rotate_and_mask() {
    Fixture fixture;
    fixture.emit({
        0x3C608000u,  // lis r3, 0x8000
        0x60630001u,  // ori r3, r3, 1
        0x546007FEu,  // rlwinm r0, r3, 0, 31, 31
    });
    fixture.run(3);
    require(fixture.cpu.gpr[0] == 1u, "rlwinm must extract the least significant bit");
}

void test_carry_and_overflow() {
    Fixture fixture;
    fixture.emit({
        0x3C60FFFFu,  // lis r3, 0xFFFF
        0x6063FFFFu,  // ori r3, r3, 0xFFFF
        0x38800001u,  // li r4, 1
        0x7C032014u,  // addc r0, r3, r4
    });
    fixture.run(4);
    require(fixture.cpu.gpr[0] == 0u, "addc must wrap");
    require((fixture.cpu.xer & kXerCA) != 0u, "addc must set the carry bit");
}

// Paired-single quantised loads are how the game streams vertex data.
void test_paired_single_quantised_load() {
    Fixture fixture;
    // GQR1: load type 5 (u16), load scale 1 -> divide by two.
    fixture.cpu.spr[SPR_GQR0 + 1] = (5u << 16) | (1u << 24);
    fixture.bus.write16(0x80004000u, 200);
    fixture.bus.write16(0x80004002u, 100);
    fixture.emit({
        0x3C608000u,  // lis r3, 0x8000
        0x60634000u,  // ori r3, r3, 0x4000
        0xE0231000u,  // psq_l f1, 0(r3), 0, 1
    });
    fixture.run(3);
    const double first = bits_to_double(fixture.cpu.fpr[1].bits[0]);
    const double second = bits_to_double(fixture.cpu.fpr[1].bits[1]);
    require(first == 100.0, "quantised load must apply the GQR scale to the first element");
    require(second == 50.0, "quantised load must apply the GQR scale to the second element");
}

// Translation is the window itself: with MSR[IR] / MSR[DR] on, an address
// translates to itself (the page table decides whether anything is there);
// without them it does not translate at all - there is no real mode.
void test_translation_is_the_window() {
    Fixture fixture;
    u32 address = 0;
    require(fixture.cpu.translate(0x80345678u, false, false, address) && address == 0x80345678u,
            "a data address translates to itself");
    require(fixture.cpu.translate(0x80345678u, true, false, address) && address == 0x80345678u,
            "a store address translates to itself");
    require(fixture.cpu.translate(Fixture::kBase, false, true, address) && address == Fixture::kBase,
            "an instruction address translates to itself");
    fixture.cpu.set_msr(fixture.cpu.msr() & ~(kMsrIR | kMsrDR));
    require(!fixture.cpu.translate(0x80345678u, false, false, address) &&
                !fixture.cpu.translate(Fixture::kBase, false, true, address),
            "nothing translates with MSR[IR|DR] off: real mode does not exist");
}

// The game's window is flat (docs/memory-architecture-20261006): an address
// means one byte or nothing, the CPU reads the page table directly and the
// bytes are the ones GuestMemory holds at the same address. No GameCube mirror
// exists: the uncached form of an address maps nothing and an access stops.
void test_window_page_table_fast_path() {
    Fixture fixture;
    fixture.cpu.mem_write32(0x80003000u, 0x12345678u);
    require(fixture.cpu.mem_read32(0x80003000u) == 0x12345678u, "window writes must be visible to reads");
    require(fixture.bus.read32(0x80003000u) == 0x12345678u,
            "the CPU and GuestMemory see the same bytes at the same address");
    fixture.cpu.mem_write16(0x80003002u, 0xabcdu);
    require(fixture.cpu.mem_read8(0x80003000u) == 0x12u, "byte 0 of a big-endian word");
    require(fixture.cpu.mem_read8(0x80003003u) == 0xcdu, "byte 3 of a big-endian word");
    require(fixture.cpu.mem_read32(0x80003000u) == 0x1234abcdu,
            "a 32-bit read must see the overlapping 16-bit store");
    constexpr std::size_t kImageBytes = GuestMemory::kImageEnd - GuestMemory::kImageBase;
    require(fixture.bus.host_pointer(GuestMemory::kImageBase, kImageBytes) != nullptr,
            "the whole image region is one host span with native-size lengths");
    require(fixture.bus.host_pointer(GuestMemory::kImageBase, kImageBytes + 1u) == nullptr,
            "a span past its region must be refused");
    require(!fixture.bus.mapped(GcAddr(0xC0003000u), 4u),
            "no uncached mirror: the GameCube form of the address maps nothing");
    fixture.cpu.set_unmapped_access_stops(true);
    (void)fixture.cpu.mem_read32(0xC0003000u);
    require(fixture.cpu.stopped(), "an access through a GameCube mirror stops the machine");
}

// ---- native game time (docs/native-time-20261005/DESIGN.md) -----------------

// The clock moves only in advance_game_time, event by event, each event at its
// own tick; a deliverable interrupt stops it before it moves.
void test_game_clock_delivers_events_in_order() {
    System system;
    system.start();
    Cpu& cpu = system.cpu();
    std::vector<u64> services, pacing;
    system.set_host_service([&](u64 ticks) { services.push_back(ticks); });
    system.set_host_pacing([&](u64 ticks) { pacing.push_back(ticks); });
    constexpr u64 kPeriod = System::kHostServiceTicks;
    const u64 start = system.game_ticks();
    require(system.advance_game_time(start + 3u * kPeriod) &&
                system.game_ticks() == start + 3u * kPeriod,
            "the clock must reach the target");
    require(services == std::vector<u64>{start + kPeriod, start + 2u * kPeriod, start + 3u * kPeriod},
            "every host service must run at its own tick, in order");
    // The pacing ticks between them (docs/FRAME_PACING.md "Pacing on the frame grid"): every 1/300 s that is not a
    // service tick, so the 1/60 s frame boundary 675,000 is one and the service ticks (and input tapes) are unchanged.
    constexpr u64 kPace = System::kPacingTicks;
    require(start % kPeriod == 0u &&
                pacing == std::vector<u64>{start + kPace, start + 2u * kPace, start + 4u * kPace},
            "a pacing tick runs at every 1/300 s that is not a host-service tick");
    pacing.clear();
    require(system.advance_game_time(start + 675000u) && pacing.back() == start + 675000u,
            "the 1/60 s frame boundary is a pacing tick");

    cpu.set_msr(cpu.msr() & (~kMsrEE));
    const u64 armed = system.game_ticks();
    cpu.set_decrementer_delivery(true);  // as a guest mtdec does (a System starts with delivery off)
    cpu.set_decrementer(99u);  // the 0 -> 1 edge 100 ticks after the write
    require(cpu.game_clock().deadline(GameEvent::Decrementer) == armed + 100u,
            "mtdec must arm the edge value + 1 ticks out");
    require(system.advance_game_time(armed + 100u) && cpu.decrementer_pending() &&
                cpu.decrementer() == 0xFFFFFFFFu,
            "the edge must be delivered at exactly its tick, DEC reading -1");
    require(cpu.game_clock().deadline(GameEvent::Decrementer) == armed + 100u + (u64{1} << 32),
            "the next edge is a full wrap later");

    cpu.set_msr(cpu.msr() | kMsrEE);
    const u64 held = system.game_ticks();
    require(!system.advance_game_time(held + kPeriod) && system.game_ticks() == held,
            "a deliverable interrupt is the guest's to take before time moves on");
}

// A wait point advances to the NEXT event only (a loop may poll device state
// with interrupts masked and must re-read it after every event), returns at
// once when an interrupt is deliverable, and marks a journalled segment as
// unreplayable.
void test_wait_point_advances_one_event_at_a_time() {
    System system;
    system.start();
    Cpu& cpu = system.cpu();
    u64 services = 0, pacing = 0;
    system.set_host_service([&](u64) { ++services; });
    system.set_host_pacing([&](u64) { ++pacing; });
    cpu.set_msr(cpu.msr() & (~kMsrEE));
    const u64 start = system.game_ticks();
    // The first host service. The pacing tick before it (1/300 s) is not an event of the game's: a wait passes it
    // (docs/FRAME_PACING.md "Pacing on the frame grid"), so a polling loop sees the same events as without it.
    const u64 frame = (start / System::kHostServiceTicks + 1u) * System::kHostServiceTicks;
    require(cpu.game_clock().deadline(GameEvent::Frame) < frame, "a pacing tick comes before the first service");
    cpu.set_decrementer_delivery(true);  // as a guest mtdec does (a System starts with delivery off)
    cpu.set_decrementer(static_cast<u32>(frame - start - 11u));  // edge 10 ticks before the service
    cpu.idle_wait();
    require(system.game_ticks() == frame - 10u && cpu.decrementer_pending() && services == 0u && pacing == 1u,
            "a wait must advance to the next event of the game's and deliver only it (pacing ticks on the way)");
    cpu.idle_wait();
    require(system.game_ticks() == frame && services == 1u,
            "with the edge masked, the next wait moves on to the next event");
    cpu.set_msr(cpu.msr() | kMsrEE);
    const u64 held = system.game_ticks();
    cpu.idle_wait();
    require(system.game_ticks() == held && services == 1u,
            "a wait with a deliverable interrupt must return before time moves");
    cpu.set_msr(cpu.msr() & (~kMsrEE));
    system.guest_memory().journal_begin();
    cpu.idle_wait();
    require(system.guest_memory().journal_touched_device(),
            "the events a wait delivers cannot be replayed: the segment counts as device-touching");
    system.guest_memory().journal_end();
}

// Cpu::poll_point waits only at a fixed point: an iteration that left every
// register as the previous visit found it AND made no net change to memory.
void test_poll_point_waits_only_at_a_fixed_point() {
    Fixture f;
    u64 waits = 0;
    f.cpu.set_idle_handler([&](Cpu&) { ++waits; });
    constexpr u32 kSite = 0x80100700u;
    constexpr u32 kStatus = 0x80110080u;
    // A status query that returns r3 and re-stores its copy.
    const auto iterate = [&](u32 value) {
        f.cpu.gpr[3] = value;
        f.cpu.mem_write32(kStatus, value);
        f.cpu.poll_point(kSite);
    };
    iterate(1u);
    require(waits == 0u && !f.bus.watch_active(), "the first visit records the registers");
    iterate(1u);
    require(waits == 0u && f.bus.watch_active(), "unchanged registers start a watch");
    iterate(1u);
    require(waits == 1u && !f.bus.watch_active(),
            "an iteration that changed no register and no memory is a fixed point: wait");

    iterate(1u);
    iterate(2u);
    iterate(3u);
    require(waits == 1u, "an iteration that changes a register is progress");

    u32 counter = 0;
    const auto count = [&]() {
        f.cpu.gpr[3] = 7u;
        f.cpu.mem_write32(kStatus, ++counter);
        f.cpu.poll_point(kSite);
    };
    for (int i = 0; i < 5; ++i) count();
    require(waits == 1u, "an iteration that changes memory is progress, whatever the registers do");

    f.cpu.gpr[3] = 9u;
    f.cpu.poll_point(kSite);
    f.cpu.poll_point(kSite);
    require(f.bus.watch_active(), "a watch runs");
    const u64 aborts = f.bus.watch_aborts();
    for (u32 i = 0; i < 65u; ++i) f.cpu.mem_write32(0x80120000u + 4u * i, i + 1u);
    require(!f.bus.watch_active() && f.bus.watch_aborts() == aborts + 1u,
            "a watched iteration doing real work must end its watch at once");
    require(f.cpu.mem_read32(0x80120000u + 4u * 64u) == 65u, "the write that ended it still lands");

    f.cpu.poll_point(kSite);
    require(f.bus.watch_active(), "unchanged registers after real work restart the watch");
    f.cpu.poll_point(kSite);
    require(waits == 2u && !f.bus.watch_active(),
            "an iteration with nothing written under the restarted watch is a fixed point");
    f.cpu.poll_point(kSite);  // the site and registers survive a wait: watch at once
    require(f.bus.watch_active(), "a watch runs again");
    f.cpu.end_poll_tracking();
    require(!f.bus.watch_active(), "a wait or another site ends tracking");
    f.cpu.poll_point(kSite);
    f.cpu.poll_point(0x80100704u);
    f.cpu.poll_point(kSite);
    require(waits == 2u && !f.bus.watch_active(), "visits of another site restart the tracking");
}

// The one cross-thread entry into the core: a host thread stops a guest that
// spins without ever reaching a wait point, and the stop names the guest
// location.
void test_host_thread_stop_reaches_a_spinning_guest() {
    constexpr u32 kSpin = 0x80100800u;
    Fixture f;
    f.bus.write32(kSpin, 0x48000000u);  // b .
    const RecompiledCode::Entry entries[] = {{kSpin, 4u, +[](Cpu& c) {
        if (c.try_hle(kSpin)) return;
        for (;;) {
            c.pc = kSpin;
            if (c.needs_dispatch()) {
                c.leave_to_dispatcher(kSpin, FallbackReason::Yield, kSpin);
                return;
            }
        }
    }, "spin"}};
    RecompiledCode code;
    code.install(entries, 1u);
    f.cpu.attach_recompiled(&code);
    f.cpu.set_entry(kSpin);
    std::thread host([&f] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        f.cpu.request_stop_from_host(StopReason::HostRequest, "host stop test");
    });
    f.cpu.run_steps(~0ull);
    host.join();
    require(f.cpu.stop().reason == StopReason::HostRequest &&
                f.cpu.stop().detail.find("host stop test; guest pc=0x80100800") != std::string::npos,
            "the host stop must end the spin and name the guest location: " + f.cpu.stop().detail);
}

// Cpu::reset is a fresh machine: a host stop posted for a run that has ended
// must not stop the next run at its first dispatch, and the next post is
// served as usual (difftest drivers reuse one machine across vectors).
void test_reset_drops_a_posted_host_stop() {
    constexpr u32 kBody = 0x80100900u;
    Fixture f;
    f.bus.write32(kBody, 0x38600007u);          // li r3,7
    f.bus.write32((kBody + 4u), 0x4E800020u);   // blr
    const RecompiledCode::Entry entries[] = {{kBody, 8u, +[](Cpu& c) {
        if (c.try_hle(kBody)) return;
        c.gpr[3] = 7u;
        c.pc = c.lr;
    }, "body"}};
    RecompiledCode code;
    code.install(entries, 1u);
    f.cpu.attach_recompiled(&code);
    f.cpu.request_stop_from_host(StopReason::HostRequest, "stale host stop");
    f.cpu.reset();
    f.cpu.set_entry(kBody);
    f.cpu.lr = kBody + 0x100u;
    require(f.cpu.run_steps(1u) == 1u && f.cpu.gpr[3] == 7u && f.cpu.pc == kBody + 0x100u &&
                f.cpu.stop().reason != StopReason::HostRequest,
            "a host stop posted before the reset must not stop the next run: " + f.cpu.stop().detail);

    f.cpu.request_stop_from_host(StopReason::HostRequest, "fresh host stop");
    f.cpu.set_entry(kBody);
    f.cpu.gpr[3] = 0u;
    f.cpu.run_steps(1u);
    require(f.cpu.stop().reason == StopReason::HostRequest && f.cpu.gpr[3] == 0u &&
                f.cpu.stop().detail.find("fresh host stop; guest pc=0x80100900") != std::string::npos,
            "a host stop posted after the reset is served at the next dispatch: " + f.cpu.stop().detail);
}

// ---- native video (docs/native-time-20261005/STAGE2_VI.md) -----------------

// The IPL's PAL display runs from boot: one retrace per field, 625 half-lines
// of 1,296 ticks (810,000 ticks) each, every field end latching the native VI
// source; masked (VIInit has not unmasked it), the line stays low. The beam
// is the half-line within the frame.
void test_native_video_field_clock() {
    System system;
    system.start();
    Cpu& cpu = system.cpu();
    NativeVideo& video = system.video();
    const u64 start = system.game_ticks();
    require(video.enabled() && video.field_ticks() == 810000u &&
                cpu.game_clock().deadline(GameEvent::Retrace) == start + 810000u,
            "the PAL boot display ends its first field 810,000 ticks after boot");
    require(system.advance_game_time(start + 810000u) && system.game_ticks() == start + 810000u,
            "the clock reaches the first field end");
    require(video.field_index() == 1u && video.stats().fields == 1u &&
                system.interrupts().status(NativeInterrupts::Source::Vi) && !cpu.native_interrupt(),
            "the retrace latches the VI source; masked, the line stays low");
    require(cpu.game_clock().deadline(GameEvent::Retrace) == start + 2u * 810000u,
            "the next field ends one field later");
    const NativeVideo::Beam beam = video.beam(start + 810000u + 10u * 1296u + 5u);
    require(beam.frame == 0u && beam.half_line == 625u + 10u && beam.field_half_lines == 625u,
            "the beam is in the second field of frame 0, 10 half-lines in");
    require(system.advance_game_time(start + 2u * 810000u) && video.frames() == 1u,
            "two fields make a frame");
}

// Native sources are taken in the OS's priority order (the rows of
// __OSDispatchInterrupt's table at 0x805625C8, read from the game's data): VI's
// row precedes PE's and the row holding ARAM, DSP and the EXI range (lowest
// number first within it), which precedes DSP_AI's row; CP's row is last.
// SetInterruptMask applies the native sources' groups and programs no device:
// there is no processor or memory interface (stage 6).
void test_native_interrupts_follow_os_priority() {
    System system;
    system.start();
    const u32 rows[] = {0x00000100u, 0x00000040u, 0xF8000000u, 0x00000200u, 0x00000080u, 0x00003000u,
                        0x00000020u, 0x03FF8C00u, 0x04000000u, 0x00004000u, 0xFFFFFFFFu};
    for (u32 i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        system.guest_memory().write32(0x805625C8u + 4u * i, rows[i]);
    }
    Cpu& cpu = system.cpu();
    NativeInterrupts& interrupts = system.interrupts();
    using Source = NativeInterrupts::Source;
    // Every OS interrupt enabled: the DSP group and the PI group applied.
    cpu.lr = 0x80004000u;
    cpu.gpr[3] = 0x07000000u;
    cpu.gpr[4] = 0u;
    require(cpu.try_hle(0x8037620Cu) && cpu.gpr[3] == 0u, "the DSP group is applied");
    cpu.gpr[3] = 0x00007FE0u;
    cpu.gpr[4] = 0u;
    require(cpu.try_hle(0x8037620Cu) && cpu.gpr[3] == 0u, "the PI group is applied");
    require(interrupts.unmasked(Source::Vi) && interrupts.unmasked(Source::GxBreakpoint) &&
                interrupts.unmasked(Source::PeFinish),
            "PI_VI / PI_CP / PI_PE_FINISH unmask the native retrace and GX sources");
    // The memory interface's group (OS interrupts 0-4) has no source: nothing
    // is applied and no device is programmed.
    cpu.gpr[3] = 0xF8000000u;
    cpu.gpr[4] = 0u;
    require(cpu.try_hle(0x8037620Cu) && cpu.gpr[3] == 0u && !cpu.stopped(),
            "the memory interface's group applies nothing");

    require(interrupts.next_source() == Source::Count, "nothing pending, nothing to deliver");
    interrupts.raise(Source::AiDma);
    interrupts.raise(Source::DspDma);
    require(interrupts.next_source() == Source::DspDma, "ARAM's row (6) precedes DSP_AI's");
    interrupts.raise(Source::Dsp);
    require(interrupts.next_source() == Source::DspDma, "ARAM (6) precedes DSP (7) within their row");
    interrupts.raise(Source::Vi);
    require(interrupts.next_source() == Source::Vi, "VI's row precedes every other");
    interrupts.clear(Source::Vi);
    interrupts.raise(Source::PeFinish);  // OS interrupt 19, PE's row
    require(interrupts.next_source() == Source::PeFinish, "PE's row precedes ARAM's");
    interrupts.clear(Source::PeFinish);
    interrupts.clear(Source::DspDma);
    interrupts.clear(Source::Dsp);
    require(interrupts.next_source() == Source::AiDma, "DSP_AI's row is next");
    interrupts.raise(Source::GxBreakpoint);  // OS interrupt 17, the last row
    require(interrupts.next_source() == Source::AiDma, "DSP_AI's row precedes CP's");
    interrupts.clear(Source::AiDma);
    require(interrupts.next_source() == Source::GxBreakpoint, "CP's row is last");
}

// The listed platform sites (extract/platform-sites.txt) get the native
// platform's answer; another register at a listed site, or an unlisted site,
// is refused.
void test_native_video_platform_sites() {
    {
        System system;
        system.start();
        Cpu& cpu = system.cpu();
        // The loading screen's console field constant (li r0,0x412B = floor(675,675 / 40.5) us): the current
        // field in whole microseconds - the PAL boot field is 1/50 s.
        require(system.video().field_ticks() == 810000u && cpu.platform_value(0x802BEA00u) == 20000u &&
                    !cpu.stopped(), "display_field_us is the current field in microseconds");
        cpu.platform_value(0x80600000u);
        require(cpu.stopped(), "an unlisted value site is refused");
    }
    {
        // No video write site is left: the reset path's DCR writes left the
        // list with stage 6 (OSResetSystem is owned, __OSDoHotReset pruned -
        // their addresses must not appear here: a runtime literal is a
        // liveness root). An unlisted write is refused.
        System system;
        system.start();
        system.cpu().platform_write16(0x80600000u, 0xCC002002u, 0u);
        require(system.cpu().stopped() && system.video().enabled(),
                "an unlisted platform write is refused and the display keeps running");
    }
    {
        System system;
        system.start();
        system.cpu().platform_read16(0x802BEA00u, 0xCC00206Eu);
        require(system.cpu().stopped(), "a listed value site used as a register read is refused");
    }
    {
        System system;
        system.start();
        system.cpu().platform_read16(0x80600000u, 0xCC00206Eu);
        require(system.cpu().stopped(), "an unlisted site is refused");
    }
    // No VISEL read site is left: __CARDFormatRegionAsync's (0x80391910) left the list with the CARD library (the
    // save store owns the job runner that called it), the OS font library's three left with os-font (OSGetFontEncode
    // owned, OSInitFont / OSGetFontTexture never entered), and SISetSamplingRate's VICLK read left with its function,
    // which lift L1 deleted with the rest of the serial-interface library. The test uses 0x80600000 (outside the
    // DOL's text) as the unlisted site: naming a code address here would make it a liveness root and keep the
    // deleted code in the image. tools/lift/verify_l1_input.py --pruned checks it is gone.
}

// The field clock survives a savestate block round trip exactly.
void test_native_video_savestate_round_trip() {
    System source;
    source.start();
    require(source.advance_game_time(source.game_ticks() + 3u * 810000u + 7u), "three fields");
    std::vector<u8> block;
    source.video().save_state(block);
    System target;
    const u8* cursor = block.data();
    require(target.video().load_state(cursor, block.data() + block.size()) &&
                cursor == block.data() + block.size(),
            "the block loads whole");
    require(target.video().enabled() && target.video().field_index() == 3u &&
                target.video().field_ticks() == 810000u &&
                target.video().beam(source.game_ticks()).half_line ==
                    source.video().beam(source.game_ticks()).half_line,
            "the loaded field clock is the saved one");
}

// ---- controller input ------------------------------------------------------
// NativePad and its PAD / SI owners were deleted by lift L1
// (docs/lift-20261006/L1_INPUT.md): controller input is the lifted peripheral
// layer, tested in lift_peripheral_tests (the PC implementation) and
// lift_sites_reference_tests (the generated lowered call sites).

// ---------------------------------------------------------------------------
// Native GX synchronisation (docs/native-time-20261005/STAGE4_GX_SYNC.md)
// ---------------------------------------------------------------------------

constexpr u32 kGxSda = 0x805EC500u;
constexpr u32 kGxSda2 = 0x805FA780u;
constexpr u32 kGxData = 0x81110000u;      // a __GXData the tests own
constexpr u32 kGxFifo = 0x81111000u;      // a GXFifoObj bound as CPU and GP FIFO
constexpr u32 kGxOtherFifo = 0x81111100u; // one never bound
constexpr u32 kGxPointers = 0x81111200u;  // GXGetFifoPtrs' rd / wr out words
constexpr u32 kGxShutdownAddress = 0x80393754u;
constexpr u32 kGxSetCpuFifoAddress = 0x80394BDCu;
constexpr u32 kGxSetGpFifoAddress = 0x80394D04u;
constexpr u32 kGxGetFifoPtrsAddress = 0x80394EA4u;
constexpr u32 kGxEnableBreakPtAddress = 0x80394F80u;
constexpr u32 kGxDisableBreakPtAddress = 0x80395034u;
constexpr u32 kGxFifoInitAddress = 0x80395088u;
constexpr u32 kGxReadDrawSyncAddress = 0x803964DCu;
constexpr u32 kGxPeInitAddress = 0x80396840u;
constexpr u32 kGxCurrentThread = 0x81200000u;  // what OSGetCurrentThread returns here

// __GXData zeroed, the OS mask all masked (as __OSInterruptInit leaves it), a
// FIFO object as GXInitFifoBase(base 0x81300000, 256 KiB) leaves it with the
// write pointer 0x40 bytes in.
void prepare_gx_guest(System& system) {
    Cpu& cpu = system.cpu();
    cpu.gpr[13] = kGxSda;
    cpu.gpr[2] = kGxSda2;
    cpu.mem_write32(kGxSda2 + 0xFD8u, kGxData);
    for (u32 i = 0; i < 0x600u; i += 4u) cpu.mem_write32(kGxData + i, 0u);
    for (u32 i = 0x5E08u; i < 0x5E58u; i += 4u) cpu.mem_write32(kGxSda + i, 0u);
    cpu.mem_write32(0x800000C4u, 0xFFFFFFFFu);
    cpu.mem_write32(0x800000C8u, 0u);
    cpu.mem_write32(0x800000E4u, kGxCurrentThread);
    const u32 fifo_words[8] = {0x81300000u, 0x8133FFFCu, 0x00040000u, 0x0003C000u,
                               0x00020000u, 0x81300000u, 0x81300040u, 0x00000040u};
    for (u32 i = 0; i < 8u; ++i) cpu.mem_write32(kGxFifo + 4u * i, fifo_words[i]);
    const u32 other_words[8] = {0x81400000u, 0x8140FFFCu, 0x00010000u, 0x0000C000u,
                                0x00008000u, 0x81408000u, 0x81400020u, 0u};
    for (u32 i = 0; i < 8u; ++i) cpu.mem_write32(kGxOtherFifo + 4u * i, other_words[i]);
}

u32 call_gx(Cpu& cpu, u32 address, u32 r3 = 0u, u32 r4 = 0u, u32 r5 = 0u) {
    cpu.gpr[3] = r3;
    cpu.gpr[4] = r4;
    cpu.gpr[5] = r5;
    cpu.lr = 0x80004000u;
    require(cpu.try_hle(address), "a native GX sync owner serves every call");
    require(!cpu.stopped(), "a native GX sync owner stopped the machine");
    return cpu.gpr[3];
}

// GXSetCPUFifo / GXSetGPFifo / GXGetFifoPtrs keep the SDK's bookkeeping - the
// FIFO globals, CPGPLinked, the CP control and clear shadows - and answer the
// positions the FIFO was bound with; nothing advances them.
void test_native_gx_fifo_binding() {
    System system;
    system.start();
    prepare_gx_guest(system);
    Cpu& cpu = system.cpu();
    call_gx(cpu, kGxSetCpuFifoAddress, kGxFifo);
    require(cpu.mem_read32(kGxSda + 0x5E20u) == kGxFifo && cpu.mem_read8(kGxSda + 0x5E2Cu) == 0u &&
                (cpu.mem_read32(kGxData + 0x08u) & 0x1Cu) == 0u,
            "GXSetCPUFifo of a FIFO that is not the GP's: unlinked, no FIFO interrupts");
    call_gx(cpu, kGxSetGpFifoAddress, kGxFifo);
    require(cpu.mem_read32(kGxSda + 0x5E24u) == kGxFifo && cpu.mem_read8(kGxSda + 0x5E2Cu) == 1u &&
                cpu.mem_read32(kGxData + 0x08u) == 0x15u && cpu.mem_read32(kGxData + 0x10u) == 0x03u,
            "GXSetGPFifo of the CPU's FIFO: linked - read enable, overflow interrupt, GP link; both clears");
    call_gx(cpu, kGxSetCpuFifoAddress, kGxFifo);
    require(cpu.mem_read8(kGxSda + 0x5E2Cu) == 1u && cpu.mem_read32(kGxData + 0x08u) == 0x15u,
            "GXSetCPUFifo of the GP's FIFO keeps the link");
    require(system.gx_sync().cpu_fifo_write() == 0x81300040u && system.gx_sync().gp_fifo_read() == 0x81300000u &&
                system.gx_sync().gp_fifo_count() == 0x40u,
            "the bound positions: the CPU write position and the GP read position and distance");
    cpu.mem_write32(kGxFifo + 0x14u, 0x81330000u);
    cpu.mem_write32(kGxFifo + 0x18u, 0x81310000u);
    cpu.mem_write32(kGxFifo + 0x1Cu, 0x1234u);
    call_gx(cpu, kGxGetFifoPtrsAddress, kGxFifo, kGxPointers, kGxPointers + 4u);
    require(cpu.mem_read32(kGxFifo + 0x14u) == 0x81300000u && cpu.mem_read32(kGxFifo + 0x18u) == 0x81300040u &&
                cpu.mem_read32(kGxFifo + 0x1Cu) == 0x40u,
            "GXGetFifoPtrs refreshes the FIFO object from the bound positions");
    require(cpu.mem_read32(kGxPointers) == 0x81300000u && cpu.mem_read32(kGxPointers + 4u) == 0x81300040u,
            "GXGetFifoPtrs returns the read and write pointers");
    call_gx(cpu, kGxGetFifoPtrsAddress, kGxOtherFifo, kGxPointers, kGxPointers + 4u);
    require(cpu.mem_read32(kGxOtherFifo + 0x1Cu) == 0x81400020u - 0x81408000u + 0x10000u &&
                cpu.mem_read32(kGxPointers) == 0x81408000u && cpu.mem_read32(kGxPointers + 4u) == 0x81400020u,
            "an unbound FIFO: the distance is computed from its own pointers, wrapped by its size");
    require(system.gx_sync().stats().fifo_binds == 3u && system.gx_sync().stats().fifo_reads == 2u,
            "the binds and reads are counted");
}

// GXEnableBreakPt arms the breakpoint and it is reached at once (everything
// written before it has been consumed): the native breakpoint source is
// raised. GXDisableBreakPt withdraws it. GXSetGPFifo's enable pulse reaches an
// armed breakpoint again only while its interrupt is enabled in the shadow.
void test_native_gx_breakpoint_raise_and_disable() {
    System system;
    system.start();
    prepare_gx_guest(system);
    Cpu& cpu = system.cpu();
    NativeInterrupts& interrupts = system.interrupts();
    using Source = NativeInterrupts::Source;
    cpu.mem_write32(kGxSda + 0x5E30u, 7u);
    call_gx(cpu, kGxFifoInitAddress);
    require(interrupts.unmasked(Source::GxBreakpoint) && (cpu.mem_read32(0x800000C4u) & 0x4000u) == 0u,
            "__GXFifoInit unmasks PI_CP: the native breakpoint source");
    require(cpu.mem_read32(kGxSda + 0x5E28u) == kGxCurrentThread && cpu.mem_read32(kGxSda + 0x5E30u) == 0u &&
                cpu.mem_read32(kGxSda + 0x5E20u) == 0u && cpu.mem_read32(kGxSda + 0x5E24u) == 0u,
            "__GXFifoInit: the current thread recorded, no overflow suspend, no FIFOs");
    call_gx(cpu, kGxEnableBreakPtAddress, 0x81300040u);
    require(cpu.mem_read32(kGxData + 0x08u) == 0x23u && cpu.mem_read32(kGxSda + 0x5E3Cu) == 0x81300040u,
            "GXEnableBreakPt: read, breakpoint and breakpoint-interrupt enables; __GXCurrentBP");
    require(interrupts.status(Source::GxBreakpoint) && system.gx_sync().stats().breakpoints == 1u,
            "the armed breakpoint is reached at once");
    call_gx(cpu, kGxDisableBreakPtAddress);
    require(cpu.mem_read32(kGxData + 0x08u) == 0x01u && cpu.mem_read32(kGxSda + 0x5E3Cu) == 0u &&
                !interrupts.status(Source::GxBreakpoint),
            "GXDisableBreakPt clears both enables and withdraws the reached breakpoint");
    call_gx(cpu, kGxEnableBreakPtAddress, 0x81300040u);
    interrupts.clear(Source::GxBreakpoint);
    call_gx(cpu, kGxSetGpFifoAddress, kGxFifo);
    require(interrupts.status(Source::GxBreakpoint) && system.gx_sync().stats().breakpoints == 3u,
            "GXSetGPFifo's enable pulse reaches an armed breakpoint again");
    interrupts.clear(Source::GxBreakpoint);
    cpu.mem_write32(kGxData + 0x08u, cpu.mem_read32(kGxData + 0x08u) & ~0x20u);  // the delivery's work
    call_gx(cpu, kGxSetGpFifoAddress, kGxFifo);
    require(!interrupts.status(Source::GxBreakpoint),
            "a delivered breakpoint (its interrupt disabled in the shadow) is not reached again");
}

// GXSetDrawSync's and GXDrawDone's lowered sites (extract/gx-sites.txt).
constexpr u32 kGxTokenIrqSite = 0x8039645Cu;  // BP 0x48 payload
constexpr u32 kGxTokenSite = 0x80396474u;     // BP 0x47 payload
constexpr u32 kGxDrawDoneSite = 0x80396510u;  // BP 0x45 payload

// The lowered draw-sync sites (gekko/gx_sites.h, through System's router):
// GXReadDrawSync answers the last
// token at once; a token with interrupt (BP 0x48) raises nothing - its handler
// only served a draw-sync callback this image never registers - and the finish
// interrupt is raised once __GXPEInit enabled it.
void test_native_gx_draw_sync_token_and_draw_done() {
    System system;
    system.start();
    prepare_gx_guest(system);
    Cpu& cpu = system.cpu();
    NativeInterrupts& interrupts = system.interrupts();
    using Source = NativeInterrupts::Source;
    system.cpu().gx_draw_sync_token(kGxTokenIrqSite, 0x0007u, true);
    require(call_gx(cpu, kGxReadDrawSyncAddress) == 7u && system.gx_sync().stats().token_interrupts == 0u,
            "a token before __GXPEInit is read back and its interrupt request is not enabled");
    cpu.mem_write32(kGxSda + 0x5E4Cu, 0x81000000u);
    cpu.mem_write32(kGxSda + 0x5E50u, 0x81000100u);
    call_gx(cpu, kGxPeInitAddress);
    require(cpu.mem_read32(kGxSda + 0x5E4Cu) == 0u && cpu.mem_read32(kGxSda + 0x5E50u) == 0u,
            "__GXPEInit initialises FinishQueue");
    require(interrupts.unmasked(Source::PeFinish) && (cpu.mem_read32(0x800000C4u) & 0x3000u) == 0u &&
                system.gx_sync().token_interrupt_enabled() && system.gx_sync().finish_interrupt_enabled(),
            "__GXPEInit unmasks PI_PE_TOKEN / PI_PE_FINISH in the OS mask and enables both interrupts");
    system.cpu().gx_draw_sync_token(kGxTokenSite, 0x1234u, false);
    require(call_gx(cpu, kGxReadDrawSyncAddress) == 0x1234u && system.gx_sync().stats().token_interrupts == 0u,
            "BP 0x47: the token without an interrupt request");
    system.cpu().gx_draw_sync_token(kGxTokenIrqSite, 0x1235u, true);
    require(call_gx(cpu, kGxReadDrawSyncAddress) == 0x1235u && system.gx_sync().stats().token_interrupts == 1u &&
                !cpu.native_interrupt() && !cpu.stopped(),
            "BP 0x48: the token, an interrupt request counted, nothing raised");
    require(!interrupts.status(Source::PeFinish), "no draw-done yet");
    system.cpu().gx_draw_done(kGxDrawDoneSite, 0x45000002u);
    require(interrupts.status(Source::PeFinish), "BP 0x45 = 2: the pipe is drained, the finish interrupt");
    const NativeGxSync::Stats& stats = system.gx_sync().stats();
    require(stats.tokens == 3u && stats.token_interrupts == 1u && stats.draw_dones == 1u &&
                stats.draw_sync_reads == 3u,
            "the commands and reads are counted");
}

// ---- phase 4: the GX pipe sites and the immediate geometry builder -----------
// (docs/native-time-20261005/GX_DATA_PLANE.md)

void test_vertex_format_strides() {
    // Position direct, three float elements; colour 0 direct RGBA8.
    require(decode_vertex_format((1u << 9) | (1u << 13), 0u, (1u << 0) | (4u << 1) | (1u << 13) | (5u << 14), 0u,
                                 0u).stride == 16u,
            "three floats plus an RGBA8 colour is a sixteen byte vertex");
    // Normal as 8-bit index, NBT + INDEX3 (VAT_A bits 9 and 31): three indices.
    require(decode_vertex_format(2u << 11, 0u, (1u << 9) | (1u << 10) | (1u << 31), 0u, 0u).stride == 3u,
            "INDEX3 NBT with 8-bit indices is three bytes per vertex");
    // Colour 0 direct, COLCNT=RGB, COLFMT=RGBA8: still four bytes.
    require(decode_vertex_format(1u << 13, 0u, 5u << 14, 0u, 0u).stride == 4u,
            "COLFMT RGBA8 is four bytes even when COLCNT is RGB");
    // The format's own VAT: three floats vs three signed 16-bit values.
    require(decode_vertex_format(1u << 9, 0u, (1u << 0) | (4u << 1), 0u, 0u).stride == 12u,
            "three floats is twelve bytes");
    require(decode_vertex_format(1u << 9, 0u, (1u << 0) | (3u << 1) | (8u << 4), 0u, 0u).stride == 6u,
            "three s16 is six bytes");
}


struct RecordedPrimitive {
    u8 opcode = 0;
    u32 count = 0;
    u32 stride = 0;
    u32 vat_a = 0;
    bool null_payload = false;
    std::vector<u8> payload;
};

struct RecordingImmediateSink final : ImmediateSink {
    std::vector<RecordedPrimitive> primitives;
    void on_primitive(u8 opcode, u32 vertex_count, const VertexFormat& format, u32 vat_a, u32, u32,
                      const u8* payload, std::size_t payload_bytes) override {
        RecordedPrimitive p;
        p.opcode = opcode;
        p.count = vertex_count;
        p.stride = format.stride;
        p.vat_a = vat_a;
        p.null_payload = payload == nullptr;
        if (payload != nullptr) p.payload.assign(payload, payload + payload_bytes);
        primitives.push_back(std::move(p));
    }
};

// __GXData behind the GX library's pointer (r2 + 0xFD8), as GXInit leaves it:
// format 1 = position xyz f32 + colour 0 RGBA8 (16 bytes), format 2 = xyz s16.
constexpr u32 kTestGxData = 0x805A2F60u;
constexpr u32 kTestVatA1 = (1u << 0) | (4u << 1) | (1u << 13) | (5u << 14);
struct ImmediateMachine {
    GuestMemory bus;
    Cpu cpu{bus};
    ImmediateMachine() {
        cpu.reset();
        cpu.gpr[2] = 0x805FA780u;
        bus.write32((0x805FA780u + 0xFD8u), kTestGxData);
        bus.write32((kTestGxData + 0x14u), (1u << 9) | (1u << 13));
        bus.write32((kTestGxData + 0x18u), 0u);
        bus.write32((kTestGxData + 0x1Cu + 4u), kTestVatA1);
        bus.write32((kTestGxData + 0x1Cu + 8u), (1u << 0) | (3u << 1));
    }
};

void test_native_immediate_builds_primitives() {
    ImmediateMachine m;
    NativeImmediate immediate(m.cpu);
    RecordingImmediateSink sink;
    immediate.set_sink(&sink);
    immediate.primitive_opcode(0x80001000u, 0x91u);  // triangles | format 1
    immediate.primitive_count(0x80001004u, 2u);
    require(!immediate.idle(), "a primitive with payload stays open after its count");
    for (u32 word = 0; word < 8u; ++word) {
        require(sink.primitives.empty(), "the primitive completes only with its last byte");
        immediate.vertex(0x80002000u, 0x3F800000u + word, 4u);
    }
    require(sink.primitives.size() == 1u && immediate.idle(), "32 bytes complete two 16-byte vertices");
    const RecordedPrimitive& p = sink.primitives[0];
    require(p.opcode == 0x91u && p.count == 2u && p.stride == 16u && p.vat_a == kTestVatA1 &&
                p.payload.size() == 32u && p.payload[0] == 0x3Fu && p.payload[3] == 0x00u &&
                p.payload[31] == 0x07u,
            "the payload is the stored bytes, big-endian, in order, with the shadow layout");
    // Mixed widths append exactly their bytes: format 2 is xyz s16 plus
    // colour 0 RGB565 (the descriptor enables colour 0), 8 bytes a vertex.
    immediate.primitive_opcode(0x80001000u, 0x92u);
    immediate.primitive_count(0x80001004u, 1u);
    immediate.vertex(0x80002000u, 0x1122u, 2u);
    immediate.vertex(0x80002004u, 0x33u, 1u);
    immediate.vertex(0x80002008u, 0x44u, 1u);
    immediate.vertex(0x8000200Cu, 0x5566u, 2u);
    require(sink.primitives.size() == 1u, "6 of 8 bytes leave the s16 vertex open");
    immediate.vertex(0x8000200Cu, 0x7788u, 2u);
    require(sink.primitives.size() == 2u && sink.primitives[1].stride == 8u &&
                sink.primitives[1].payload == std::vector<u8>({0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}),
            "1-, 2- and 4-byte stores append their own bytes");
    // A zero-count primitive is delivered at its count, with no payload.
    immediate.primitive_opcode(0x80001000u, 0x91u);
    immediate.primitive_count(0x80001004u, 0u);
    require(sink.primitives.size() == 3u && sink.primitives[2].null_payload && immediate.idle(),
            "an empty primitive is delivered at once");
    require(!m.cpu.stopped() && immediate.stats().primitives == 3u && immediate.stats().empty == 1u,
            "three primitives, one empty, no stop");
}

void test_native_immediate_fails_closed() {
    const auto stops = [](const char* label, const char* expected, auto&& steps) {
        ImmediateMachine m;
        NativeImmediate immediate(m.cpu);
        steps(immediate);
        require(m.cpu.stopped() && m.cpu.stop().detail.find(expected) != std::string::npos,
                std::string(label) + ": " + m.cpu.stop().detail);
    };
    stops("vertex data first", "no open primitive", [](NativeImmediate& i) { i.vertex(0x80002000u, 1u, 4u); });
    stops("header while incomplete", "previous primitive is incomplete", [](NativeImmediate& i) {
        i.primitive_opcode(0x80001000u, 0x91u);
        i.primitive_count(0x80001004u, 1u);
        i.vertex(0x80002000u, 1u, 4u);
        i.primitive_opcode(0x80001000u, 0x91u);
    });
    stops("count with no header", "with no primitive header", [](NativeImmediate& i) {
        i.primitive_count(0x80001004u, 1u);
    });
    stops("not a draw opcode", "not a draw opcode", [](NativeImmediate& i) { i.primitive_opcode(0x80001000u, 0x61u); });
    stops("past the end", "past the primitive's end", [](NativeImmediate& i) {
        i.primitive_opcode(0x80001000u, 0x92u);  // 8-byte vertices
        i.primitive_count(0x80001004u, 1u);
        i.vertex(0x80002000u, 1u, 4u);
        i.vertex(0x80002000u, 1u, 2u);
        i.vertex(0x80002000u, 1u, 4u);
    });
}

void test_native_immediate_savestate_mid_primitive() {
    ImmediateMachine m;
    NativeImmediate before(m.cpu);
    before.primitive_opcode(0x80001000u, 0x91u);
    before.primitive_count(0x80001004u, 2u);
    for (u32 word = 0; word < 3u; ++word) before.vertex(0x80002000u, 0x40000000u + word, 4u);
    std::vector<u8> blob;
    before.save_state(blob);
    NativeImmediate after(m.cpu);
    RecordingImmediateSink sink;
    after.set_sink(&sink);
    const u8* cursor = blob.data();
    require(after.load_state(cursor, blob.data() + blob.size()) && cursor == blob.data() + blob.size(),
            "the open primitive loads and consumes its whole block");
    require(!after.idle(), "a primitive open at the save is open after the load");
    for (u32 word = 3; word < 8u; ++word) after.vertex(0x80002000u, 0x40000000u + word, 4u);
    require(sink.primitives.size() == 1u && sink.primitives[0].payload.size() == 32u &&
                sink.primitives[0].payload[0] == 0x40u && sink.primitives[0].payload[31] == 0x07u,
            "the restored primitive completes with the bytes from before and after the save");
    NativeImmediate idle(m.cpu);
    blob.clear();
    idle.save_state(blob);
    blob[0] = 2u;  // claim an open primitive with an empty payload: 0 of 0 bytes is not open
    cursor = blob.data();
    NativeImmediate refused(m.cpu);
    require(!refused.load_state(cursor, blob.data() + blob.size()),
            "an 'open' primitive whose payload is already complete is refused");
}

constexpr u32 kStb = 38u, kStw = 36u, kStwu = 37u, kSth = 44u;
constexpr u32 store_word(u32 primary, u32 s, u32 a, s32 displacement) {
    return (primary << 26) | (s << 21) | (a << 16) | (static_cast<u32>(displacement) & 0xffffu);
}

// Stage 6: the interpreter (the oracle's replay) applies the generated
// platform-site table to the same words the generator lowered
// (gekko/platform_sites.h): a listed lhz is the native platform's answer, a
// listed sth its effect, a dead sth nothing - never a bus access. Without the
// table, or at a listed address holding another word, the same access is an
// unserved one, which the production policy stops.
void test_cpu_platform_sites_in_the_interpreter() {
    const u32 base = Fixture::kBase;
    constexpr u32 kLhz = 40u;
    const u32 w0 = store_word(kLhz, 4, 3, 0x206E);  // lhz r4,0x206e(r3): a listed read
    const u32 w1 = store_word(kSth, 5, 3, 0x4010);  // sth r5,0x4010(r3): dead
    const u32 w2 = store_word(kSth, 6, 3, 0x0006);  // sth r6,6(r3): a listed write
    const PlatformSiteRecord sites[] = {{base, w0, false}, {base + 4u, w1, true}, {base + 8u, w2, false}};
    {
        Fixture fixture;
        std::vector<std::string> calls;
        fixture.cpu.set_platform_sites(
            [&calls](Cpu&, u32 site, u32 effective) {
                char line[64];
                std::snprintf(line, sizeof(line), "read %08X %08X", site, effective);
                calls.emplace_back(line);
                return u16{0x1234u};
            },
            [&calls](Cpu&, u32 site, u32 effective, u16 value) {
                char line[64];
                std::snprintf(line, sizeof(line), "write %08X %08X %X", site, effective, value);
                calls.emplace_back(line);
            });
        fixture.cpu.set_platform_site_table(sites, 3u);
        fixture.cpu.set_unmapped_access_stops(true);
        fixture.emit({w0, w1, w2});
        fixture.cpu.gpr[3] = 0xCC000000u;
        fixture.cpu.gpr[5] = 0xFFu;
        fixture.cpu.gpr[6] = 0x2u;
        fixture.run(3u);
        require(!fixture.cpu.stopped() && fixture.cpu.gpr[4] == 0x1234u, "the listed read is the platform's answer");
        char expected_read[64], expected_write[64];
        std::snprintf(expected_read, sizeof(expected_read), "read %08X CC00206E", base);
        std::snprintf(expected_write, sizeof(expected_write), "write %08X CC000006 2", base + 8u);
        require(calls == std::vector<std::string>({expected_read, expected_write}),
                "the dead store reaches nothing, the listed write the platform");
        require(fixture.bus.diagnostics().unmapped_reads == 0u && fixture.bus.diagnostics().unmapped_writes == 0u &&
                    fixture.cpu.unmapped_accesses_stopped() == 0u,
                "no listed site reaches the bus");
    }
    {
        Fixture fixture;
        fixture.cpu.set_unmapped_access_stops(true);
        fixture.emit({w1});
        fixture.cpu.gpr[3] = 0xCC000000u;
        fixture.run(1u);
        require(fixture.cpu.stopped() && fixture.cpu.stop().detail.find("the memory interface") != std::string::npos,
                "without the table the dead store is an unserved access: " + fixture.cpu.stop().detail);
    }
    {
        Fixture fixture;
        fixture.cpu.set_platform_site_table(sites, 3u);
        fixture.cpu.set_unmapped_access_stops(true);
        fixture.emit({store_word(kSth, 7, 3, 0x4010)});  // another word at the listed read's address
        fixture.cpu.gpr[3] = 0xCC000000u;
        fixture.run(1u);
        require(fixture.cpu.stopped(), "a word the table does not hold is not that site");
    }
}

void test_cpu_gx_sites_in_the_interpreter() {
    const u32 base = Fixture::kBase;
    const u32 w0 = store_word(kStb, 5, 3, -0x8000);   // stb r5,-0x8000(r3): dead (a BP opcode)
    const u32 w1 = store_word(kStw, 6, 3, -0x8000);   // stw r6: vertex
    const u32 w2 = store_word(kStwu, 0, 4, -0x8000);  // stwu r0,-0x8000(r4): dead, keeps r4 = EA
    const u32 w3 = store_word(kSth, 7, 3, -0x8000);   // sth r7: primitive count
    const u32 w4 = store_word(kStw, 8, 3, -0x8000);   // stw r8: token with interrupt
    const GxSiteRecord sites[] = {{base, w0, GxSiteFamily::Dead},
                                  {base + 4u, w1, GxSiteFamily::Vertex},
                                  {base + 8u, w2, GxSiteFamily::Dead},
                                  {base + 12u, w3, GxSiteFamily::PrimitiveCount},
                                  {base + 16u, w4, GxSiteFamily::DrawSyncTokenIrq}};
    {
        Fixture fixture;
        RecordingGxSink sink;
        fixture.cpu.set_gx_sites(sites, 5u, &sink);
        fixture.emit({w0, w1, w2, w3, w4});
        fixture.cpu.gpr[3] = 0xCC010000u;
        fixture.cpu.gpr[4] = 0xCC010000u;
        fixture.cpu.gpr[5] = 0x61u;
        fixture.cpu.gpr[6] = 0x12345678u;
        fixture.cpu.gpr[7] = 0x0003u;
        fixture.cpu.gpr[8] = 0x48001234u;
        const u64 unmapped = fixture.bus.diagnostics().unmapped_writes;
        fixture.run(5u);
        char expected_vertex[64], expected_count[64], expected_token[64];
        std::snprintf(expected_vertex, sizeof(expected_vertex), "vertex %08X 12345678 4", base + 4u);
        std::snprintf(expected_count, sizeof(expected_count), "count %08X 3 0", base + 12u);
        std::snprintf(expected_token, sizeof(expected_token), "token %08X 1234 1", base + 16u);
        require(sink.calls == std::vector<std::string>({expected_vertex, expected_count, expected_token}),
                "the interpreter calls the live families and drops the dead stores");
        require(fixture.cpu.gpr[4] == 0xCC008000u, "a dead update form keeps its base update");
        require(!fixture.cpu.stopped() && fixture.cpu.unlisted_gx_stores() == 0u &&
                    fixture.bus.diagnostics().unmapped_writes == unmapped,
                "no listed site stores anywhere");
    }
    {
        // The table holds the ADDRESS but another word is there: not that site.
        Fixture fixture;
        RecordingGxSink sink;
        fixture.cpu.set_gx_sites(sites, 5u, &sink);
        fixture.emit({store_word(kStw, 9, 3, -0x8000)});
        fixture.cpu.gpr[3] = 0xCC010000u;
        fixture.run(1u);
        require(fixture.cpu.stopped() && fixture.cpu.unlisted_gx_stores() == 1u &&
                    fixture.cpu.stop().detail.find("GX site list") != std::string::npos && sink.calls.empty(),
                "an unlisted store to the pipe page stops with its location: " + fixture.cpu.stop().detail);
    }
    {
        // Without a sink (unit fixtures) the page is ordinary bus space.
        Fixture fixture;
        fixture.bus.set_trace_unmapped(false);
        const u64 unmapped = fixture.bus.diagnostics().unmapped_writes;
        fixture.cpu.mem_write32(0xCC008000u, 0u);
        require(!fixture.cpu.stopped() && fixture.bus.diagnostics().unmapped_writes == unmapped + 1u,
                "with no sink a pipe-page store is an ordinary unmapped write");
    }
}

// The generated form of `stb r5,-0x8000(r4) ; stw r6,-0x8000(r4) ; blr` with
// both sites dead: nothing, then the return.
void verify_dead_gx_sites(Cpu& c) {
    if (c.try_hle(kVerifyCutA)) return;
    c.pc = c.lr & ~3u;
}

// ... with the second site vertex data.
void verify_live_gx_site(Cpu& c) {
    if (c.try_hle(kVerifyCutA)) return;
    c.gx_vertex(kVerifyCutA + 4u, c.gpr[6], 4u);
    c.pc = c.lr & ~3u;
}

void test_verify_recomp_gx_sites() {
    const u32 w0 = store_word(kStb, 5, 4, -0x8000);
    const u32 w1 = store_word(kStw, 6, 4, -0x8000);
    const auto run = [&](GxSiteFamily second, RecompiledCode::Fn native, RecordingGxSink& sink, Fixture& fixture) {
        const GxSiteRecord sites[] = {{kVerifyCutA, w0, GxSiteFamily::Dead}, {kVerifyCutA + 4u, w1, second}};
        fixture.cpu.set_gx_sites(sites, 2u, &sink);
        fixture.bus.write32(kVerifyCutA, w0);
        fixture.bus.write32((kVerifyCutA + 4u), w1);
        fixture.bus.write32((kVerifyCutA + 8u), 0x4E800020u);  // blr
        fixture.bus.write32(kVerifyCutEnd, 0x48000000u);       // b .
        fixture.cpu.lr = kVerifyCutEnd;
        const RecompiledCode::Entry entries[] = {{kVerifyCutA, 12u, native, "gx sites"}};
        RecompiledCode code;
        code.install(entries, 1u);
        fixture.cpu.attach_recompiled(&code);
        fixture.cpu.set_verify_recompiled(true);
        fixture.cpu.gpr[4] = 0xCC010000u;
        fixture.cpu.gpr[5] = 0x61u;
        fixture.cpu.gpr[6] = 0x2aabcdefu;
        fixture.cpu.set_entry(kVerifyCutA);
        fixture.run(1u);
        fixture.cpu.set_gx_sites(nullptr, 0u, nullptr);
    };
    {
        Fixture fixture;
        RecordingGxSink sink;
        run(GxSiteFamily::Dead, verify_dead_gx_sites, sink, fixture);
        const Cpu::VerifyReport& report = fixture.cpu.verify_report();
        require(report.verified == 1u && report.mismatches == 0u && report.skipped_device == 0u &&
                    sink.calls.empty() && !fixture.cpu.stopped(),
                "a segment whose only pipe stores are dead sites is replayed and verified");
    }
    {
        Fixture fixture;
        RecordingGxSink sink;
        run(GxSiteFamily::Vertex, verify_live_gx_site, sink, fixture);
        const Cpu::VerifyReport& report = fixture.cpu.verify_report();
        require(report.skipped_device == 1u && report.verified == 0u && report.mismatches == 0u &&
                    sink.calls.size() == 1u,
                "a segment that reached a live site keeps the native result and makes its call once");
    }
}

// The layout objects' range owner: the original's single-precision limits for
// every range byte, the boundary itself in all four leaves, and the listener
// told of each admitted spawn and of the range test that closes its window.
void test_native_object_range_keeps_the_original_boundary() {
    for (u32 range = 0; range < 256u; ++range) {
        // 0x800CA970: 10000.0f * single(range * range).
        const float original = 10000.0f * static_cast<float>(range * range);
        require(float_to_bits(NativeObjectRange::in_range_limit(range)) == float_to_bits(original),
                "in-range limit is the original's");
        // 0x800C9D54: fmadds(100.0f, single(range), margin), rounded once.
        const float fused = static_cast<float>(std::fma(100.0, static_cast<double>(range), 50.0));
        require(float_to_bits(NativeObjectRange::out_of_range_radius(range, 50.0f)) == float_to_bits(fused),
                "out-of-range radius is the original's");
    }
    // Scratch guest memory as one base plus offsets (a literal guest address in
    // the runtime sources becomes a liveness root).
    constexpr u32 sda = 0x805EC500u, entry = 0x80300000u, slot = entry + 0x1000u, point = entry + 0x2000u;
    constexpr u32 players = entry + 0x3000u, data = players + 0x100u, player = players + 0x200u, out = entry + 0x4000u;
    constexpr u32 manager = entry + 0x5000u, factories = manager + 0x100u, factory = manager + 0x200u;
    Fixture f;
    Cpu& c = f.cpu;
    c.gpr[13] = sda;
    c.set_msr(c.msr() | kMsrFP);
    NativeObjectRange owner(c);
    owner.install();
    struct Listener final : NativeObjectRange::Listener {
        u32 entry = 0, factory = 0, spawns = 0, tests = 0;
        void spawn_opened(u32 e, u32 f) override {
            entry = e;
            factory = f;
            ++spawns;
        }
        void range_tested() override { ++tests; }
    } listener;
    owner.set_listener(&listener);
    c.mem_write32(slot, entry);
    c.mem_write32(slot + 0x10u, 1u);
    c.mem_write32(manager + 0x14u, factories);
    c.mem_write32(factories + 4u, factory);
    c.mem_write32(entry + 0x18u, 0u);
    c.mem_write8(entry + 0x23u, 5u);  // 500 world units
    for (u32 axis = 0; axis < 3u; ++axis) c.mem_write32(entry + axis * 4u, float_to_bits(0.0f));
    const auto in_range = [&](float x, u32 return_address) {
        c.mem_write32(point, float_to_bits(x));
        c.mem_write32(point + 4u, float_to_bits(0.0f));
        c.mem_write32(point + 8u, float_to_bits(0.0f));
        c.gpr[3] = manager;
        c.gpr[4] = slot;
        c.gpr[5] = point;
        c.lr = return_address;
        c.set_entry(0x800CA970u);
        require(c.try_hle(c.pc) && c.pc == return_address, "in_range served and returns");
        return c.gpr[3];
    };
    require(in_range(500.0f, 0x800CA914u) == 1u, "the boundary itself is in range");
    require(listener.spawns == 1u && listener.entry == entry && listener.factory == factory,
            "the listener hears the admitted spawn with its entry and factory");
    require(in_range(std::nextafter(500.0f, 1.0e9f), 0x800CA914u) == 0u, "just past it is not");
    require(in_range(std::nextafter(500.0f, 1.0e9f), 0x800CAD58u) == 0u, "a despawn test agrees");
    require(listener.spawns == 1u && listener.tests == 3u, "every range test closes the open window");
    require(owner.stats().spawns == 1u && owner.stats().spawn_tests == 2u && owner.stats().leaves == 1u,
            "spawn and leave decisions are counted by caller");
    c.mem_write32(entry + 0x18u, 0x80000u);
    require(in_range(1.0e6f, 0x800CA914u) == 1u, "an entry flagged 0x80800 is always in range");
    c.mem_write32(entry + 0x18u, 0u);

    // out_of_range: radius = 100 * range + margin, a player strictly within keeps it.
    c.mem_write32(sda + 0x3270u, float_to_bits(50.0f));
    c.mem_write32(sda + 0x2B34u, players);
    c.mem_write32(players + 0x8u, 1u);
    c.mem_write32(players + 0xCu, data);
    c.mem_write32(data, player);
    c.mem_write8(0x8057E760u + 0xB2u, 0u);
    const auto out_of_range = [&](float x) {
        c.mem_write32(player + 8u, float_to_bits(x));
        c.mem_write32(player + 12u, float_to_bits(0.0f));
        c.mem_write32(player + 16u, float_to_bits(0.0f));
        c.gpr[3] = slot;
        c.lr = 0x80004000u;
        c.set_entry(0x800C9D54u);
        require(c.try_hle(c.pc), "out_of_range served");
        return c.gpr[3];
    };
    require(out_of_range(549.0f) == 0u, "a player within range plus margin keeps the object");
    require(out_of_range(550.0f) == 1u, "on the radius is out (strict)");

    c.gpr[3] = slot;
    c.set_entry(0x800C9D20u);
    require(c.try_hle(c.pc) && c.fpr[1].bits[0] == single_to_double_bits(float_to_bits(600.0f)),
            "range radius is the distance plus 100");
    c.gpr[3] = slot;
    c.gpr[4] = out;
    c.set_entry(0x800CA200u);
    require(c.try_hle(c.pc) && c.mem_read32(out) == float_to_bits(500.0f), "range is the distance");
}

// The fade-in: a model appearing inside the previous frame's view fades in
// linearly; one entering at the edge, one drawn every frame and one that only
// blinks are drawn as the game drew them.
void test_appear_fade_only_for_models_appearing_in_view() {
    AppearFade fade;
    fade.set_duration(1000u);
    AppearFade::View view;  // at the origin looking down +z, half extents 0.5 x 0.375
    const float ahead[3] = {0.0f, 0.0f, 400.0f}, beside[3] = {800.0f, 0.0f, 400.0f}, near[3] = {0.0f, 0.0f, 100.0f};
    constexpr u32 steady = 0x10u, popper = 0x20u, edge = 0x30u, blinker = 0x40u;
    fade.end_frame(nullptr);  // no previous view: nothing fades
    require(fade.draw(steady, ahead, 100u) == 0.0f, "nothing fades without a previous view");
    require(fade.draw(blinker, ahead, 100u) == 0.0f, "nothing fades without a previous view");
    fade.end_frame(&view);
    require(fade.draw(steady, ahead, 200u) == 0.0f, "a model drawn every frame does not fade");
    require(fade.draw(popper, ahead, 5000u) == 1.0f, "a model appearing inside the view starts hidden");
    require(fade.draw(popper, ahead, 5000u) == 1.0f, "a second draw in the same frame keeps the fade");
    require(fade.draw(edge, beside, 5000u) == 0.0f, "a model outside the previous view is not faded");
    require(fade.draw(0x60u, near, 5000u) == 0.0f, "a model near the eye (the player's) is not faded");
    for (int frame = 0; frame < 5; ++frame) fade.end_frame(&view);  // the blinker is hidden for 5 frames
    require(fade.draw(blinker, ahead, 5100u) == 0.0f, "a short blink does not fade");
    require(fade.draw(popper, ahead, 5250u) == 0.75f, "a quarter of the way in, three quarters hidden");
    fade.end_frame(&view);
    require(fade.draw(popper, ahead, 6000u) == 0.0f, "a finished fade draws normally");
    for (u64 frame = 0; frame <= AppearFade::kAbsentFrames; ++frame) fade.end_frame(&view);
    require(fade.draw(popper, ahead, 9000u) == 1.0f, "absent for a second, it appears again with a fade");
    fade.set_duration(0u);
    fade.end_frame(&view);
    require(fade.draw(0x50u, ahead, 9100u) == 0.0f, "duration 0 turns the fade off");
    require(fade.stats().fades == 2u, "two appearances faded");
}

// Exercise the installed wait-point owner, not a policy helper: the same
// deadlines, published dt and setter arguments must reach the game for all
// positive console interval requests, including after an interrupted wait.
void test_native_frame_limiter_paces_events_at_one_period() {
    constexpr u32 object = 0x8057E760u, sda = 0x805EC500u;
    constexpr u32 stack = 0x81200000u, caller = 0x80004000u;
    for (const u32 hz : {50u, 60u}) {
        const u64 period_us = hz == 50u ? 20000u : 16666u;
        const u32 dt = float_to_bits(1.0e-6f * static_cast<float>(period_us));
        for (const u32 request : {1u, 2u, 60u, 0xFFFFFFFFu}) {
            for (const bool interrupt : {false, true}) {
                Fixture f;
                Cpu& c = f.cpu;
                c.gpr[13] = sda;
                c.gpr[1] = stack;
                c.set_msr(c.msr() | kMsrFP);
                c.mem_write32(0x800000F8u, 162000000u);
                c.mem_write32(object + 0x50u, hz);
                c.mem_write32(object + 0x34u, 3u);
                c.mem_write32(object + 0x10u, request);
                u64 expected_target = 0u, advances = 0u;
                bool interrupt_next = false;
                NativeFrameLimiter limiter(c, [&](u64 target) {
                    require(target == expected_target, "wait never targets a second period");
                    ++advances;
                    if (interrupt_next) {
                        interrupt_next = false;
                        c.game_clock().set_now(c.game_clock().now() + (target - c.game_clock().now()) / 2u);
                        return false;
                    }
                    c.game_clock().set_now(target);
                    return true;
                }, [&]() -> bool { throw std::runtime_error("paced frame unexpectedly used the no-wait event path"); });
                limiter.install();
                for (u64 frame = 0; frame < 600u; ++frame) {
                    // First sample at t=0; frame k ends where frame k + 1 begins:
                    // exactly (k + 1) / hz s in the 40.5 MHz time base.
                    expected_target = (frame + 1u) * (40500000u / hz);
                    interrupt_next = interrupt;
                    c.lr = caller;
                    c.set_entry(0x801E1F2Cu);
                    require(c.try_hle(c.pc), "limiter entry installed");
                    if (interrupt) {
                        require(c.pc == 0x801E20C4u, "interrupted wait publishes its continuation");
                        c.set_entry(c.pc);
                        require(c.try_hle(c.pc), "wait continuation installed");
                    }
                    require(!c.stopped() && c.pc == 0x80203D20u, "frame reaches the real frame-step setter");
                    require(c.game_clock().now() == expected_target, "frame waits one grid boundary");
                    require(c.mem_read32(sda + 0x4558u) == dt, "positive interval does not double gameplay dt");
                    require(c.gpr[3] == object + 0xCu && c.gpr[4] == 1u &&
                                c.fpr[1].bits[0] == single_to_double_bits(dt), "setter receives one unchanged timestep");
                    c.set_entry(0x801E21D8u);
                    require(c.try_hle(c.pc), "setter continuation installed");
                    require(c.pc == 0x80337DD4u && c.gpr[1] == stack && c.lr == caller,
                            "frame counters still run and caller stack is restored");
                }
                require(limiter.stats().frames == 600u && advances == (interrupt ? 1200u : 600u),
                        "every frame completed, including interrupted waits");
                require(limiter.stats().paced_at_one_period == (request > 1u ? 600u : 0u),
                        "suppressed interval requests remain visible in diagnostics");
            }
        }
    }
    // Reset/disc-error zero mode and the explicit loading bypass must still
    // progress scheduled work instead of spinning forever in zero game time.
    for (const bool bypass : {false, true}) {
        Fixture f;
        Cpu& c = f.cpu;
        c.gpr[13] = sda;
        c.gpr[1] = stack;
        c.lr = caller;
        c.set_msr(c.msr() | kMsrFP);
        c.mem_write32(0x800000F8u, 162000000u);
        c.mem_write32(object + 0x50u, 60u);
        c.mem_write32(object + 0x34u, 3u);
        c.mem_write32(object + 0x10u, bypass ? 2u : 0u);
        c.mem_write8(object + 0x14u, bypass ? 1u : 0u);
        unsigned events = 0;
        NativeFrameLimiter limiter(c, [](u64) -> bool { throw std::runtime_error("no-wait mode requested a grid wait"); },
            [&]() { ++events; c.game_clock().set_now(202500u); return true; });
        limiter.install();
        require(c.try_hle(0x801E1F2Cu) && !c.stopped() && c.pc == 0x80203D20u && events == 1u,
                "zero-time bypass progresses the next event and publishes");
    }
    // An interrupt may enter reset mode while a frame is already waiting.
    {
        Fixture f;
        Cpu& c = f.cpu;
        c.gpr[13] = sda;
        c.gpr[1] = stack;
        c.lr = caller;
        c.set_msr(c.msr() | kMsrFP);
        c.mem_write32(0x800000F8u, 162000000u);
        c.mem_write32(object + 0x50u, 60u);
        c.mem_write32(object + 0x34u, 3u);
        c.mem_write32(object + 0x10u, 2u);
        unsigned advances = 0;
        NativeFrameLimiter limiter(c, [&](u64) { ++advances; c.mem_write32(object + 0x10u, 0u); return false; },
                                   []() { return false; });
        limiter.install();
        require(c.try_hle(0x801E1F2Cu) && c.pc == 0x801E20C4u, "reset interrupts the wait");
        c.set_entry(c.pc);
        require(c.try_hle(c.pc) && !c.stopped() && c.pc == 0x80203D20u && advances == 1u,
                "reset cancels a pending wait without a second clock advance");
    }
}

// A registered draw-sync callback is refused: GXSetDrawSyncCallback's only
// caller in this image is __GXShutdown, with NULL, so nothing serves one.
void test_native_gx_draw_sync_callback_is_refused() {
    System system;
    system.start();
    prepare_gx_guest(system);
    Cpu& cpu = system.cpu();
    call_gx(cpu, kGxPeInitAddress);
    cpu.mem_write32(kGxSda + 0x5E40u, 0x81500000u);  // TokenCB
    system.cpu().gx_draw_sync_token(kGxTokenSite, 0x0009u, false);
    require(!cpu.stopped(), "a token without an interrupt request never reaches the callback");
    system.cpu().gx_draw_sync_token(kGxTokenIrqSite, 0x000Au, true);
    require(cpu.stopped() && cpu.stop().detail.find("draw-sync callback") != std::string::npos,
            "a token with interrupt and a registered draw-sync callback stops the machine with the reason");
}

// __GXShutdown, the GX OS reset function: it settles once the PE-request
// counter (0: the native GP makes no requests) has held for 10 ticks; the
// final pass clears the three callbacks and marks abtWaitPECopy.
void test_native_gx_shutdown_settles_then_aborts() {
    System system;
    system.start();
    prepare_gx_guest(system);
    Cpu& cpu = system.cpu();
    const u64 start = cpu.game_clock().now();
    require(call_gx(cpu, kGxShutdownAddress, 0u) == 0u && cpu.mem_read32(kGxSda + 0x5E18u) == 1u &&
                cpu.mem_read32(kGxSda + 0x5E08u) == 0u,
            "the first pass records the counter and the time and does not settle");
    require(call_gx(cpu, kGxShutdownAddress, 0u) == 0u, "no time passed: not settled");
    cpu.game_clock().set_now(start + 9u);
    require(call_gx(cpu, kGxShutdownAddress, 0u) == 0u, "9 ticks: not settled");
    cpu.game_clock().set_now(start + 10u);
    require(call_gx(cpu, kGxShutdownAddress, 0u) == 1u, "10 ticks with an unchanged counter: settled");
    cpu.mem_write32(kGxSda + 0x5E08u, 5u);
    require(call_gx(cpu, kGxShutdownAddress, 0u) == 0u && cpu.mem_read32(kGxSda + 0x5E08u) == 0u,
            "a changed counter restarts the settle");
    for (const u32 slot : {0x5E34u, 0x5E40u, 0x5E44u}) cpu.mem_write32(kGxSda + slot, 0x81500000u);
    call_gx(cpu, kGxEnableBreakPtAddress, 0x81300040u);
    require(call_gx(cpu, kGxShutdownAddress, 1u) == 1u, "the final pass returns TRUE");
    require(cpu.mem_read32(kGxSda + 0x5E34u) == 0u && cpu.mem_read32(kGxSda + 0x5E40u) == 0u &&
                cpu.mem_read32(kGxSda + 0x5E44u) == 0u && cpu.mem_read8(kGxData + 0x5AAu) == 1u,
            "the final pass clears the three callbacks and marks abtWaitPECopy");
    require(!system.interrupts().status(NativeInterrupts::Source::GxBreakpoint),
            "the final pass leaves no breakpoint (CP control 0)");
}

// The savestate block, and the PI's CPU FIFO registers that are gone.
void test_native_gx_sync_savestate_and_deleted_registers() {
    System source;
    source.start();
    prepare_gx_guest(source);
    call_gx(source.cpu(), kGxPeInitAddress);
    call_gx(source.cpu(), kGxSetCpuFifoAddress, kGxFifo);
    call_gx(source.cpu(), kGxSetGpFifoAddress, kGxFifo);
    source.cpu().gx_draw_sync_token(kGxTokenSite, 0x0BCDu, false);
    std::vector<u8> block;
    source.gx_sync().save_state(block);
    System target;
    const u8* cursor = block.data();
    require(target.gx_sync().load_state(cursor, block.data() + block.size()) &&
                cursor == block.data() + block.size(),
            "the block loads whole");
    const NativeGxSync& loaded = target.gx_sync();
    require(loaded.token() == 0x0BCDu && loaded.token_interrupt_enabled() && loaded.finish_interrupt_enabled() &&
                loaded.cpu_fifo_write() == 0x81300040u && loaded.gp_fifo_read() == 0x81300000u &&
                loaded.gp_fifo_count() == 0x40u,
            "the loaded state is the saved one");
    std::vector<u8> corrupt = block;
    corrupt[2] = 2u;  // the token interrupt enable
    System refused;
    cursor = corrupt.data();
    require(!refused.gx_sync().load_state(cursor, corrupt.data() + corrupt.size()),
            "an enable byte other than 0 or 1 is refused");

    // The processor interface is gone with stage 6 (its CPU FIFO registers
    // already with stage 4): a guest access to any of its registers stops the
    // machine (test_unserved_guest_access_stops covers the whole window).
    for (const u32 offset : {0x00u, 0x04u, 0x0Cu, 0x18u, 0x24u}) {
        System system;
        system.start();
        system.cpu().mem_write32(0xCC003000u + offset, 0x00200000u);
        require(system.cpu().stopped() &&
                    system.cpu().stop().detail.find("the processor interface") != std::string::npos,
                "a store to a PI register stops the machine naming the retired device: " +
                    system.cpu().stop().detail);
    }
}

// ---------------------------------------------------------------------------
// The asset read contract
// ---------------------------------------------------------------------------
//
// Ported from test_disc_source_read_parity_and_failure_contract, which was the
// only guard on the padding / failure / EOF behaviour a guest read depends on.
// Three of its five cases survive unchanged. Two CHANGE, deliberately:
//
//   * a read "below the first file" and a read "in the gap between two files"
//     were disc concepts. There is no gap: an address that no asset owns is
//     unmapped, and unmapped is a counted failure, not zeroes.
//
// Builds a directory in the layout the guest's own parser expects, plus the
// files it names, so the whole mount path is exercised.
struct TestAssetTree {
    struct File {
        std::string name;
        u32 size;
        bool on_disk = true;  // named by the directory, deleted from the tree
    };

    std::filesystem::path root;
    std::vector<File> files;

    explicit TestAssetTree(std::vector<File> entries) : files(std::move(entries)) {
        root = std::filesystem::temp_directory_path() /
               ("gekko-asset-contract-" + std::to_string(files.size()) +
                std::to_string(files.empty() ? 0u : files[0].size));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        for (const File& file : files) {
            if (!file.on_disk) continue;
            std::ofstream out(root / file.name, std::ios::binary | std::ios::trunc);
            for (u32 i = 0; i < file.size; ++i) {
                const u8 byte = static_cast<u8>((i * 31u + file.name[0]) & 0xFFu);
                out.put(static_cast<char>(byte));
            }
        }
    }
    ~TestAssetTree() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::vector<u8> directory() const {
        const u32 entries = static_cast<u32>(1u + files.size());
        std::vector<u8> names;
        std::vector<u32> name_at;
        for (const File& file : files) {
            name_at.push_back(static_cast<u32>(names.size()));
            for (char c : file.name) names.push_back(static_cast<u8>(c));
            names.push_back(0u);
        }
        std::vector<u8> table(static_cast<std::size_t>(entries) * 12u + names.size(), 0u);
        const auto put = [&](std::size_t at, u32 v) {
            table[at] = static_cast<u8>(v >> 24);
            table[at + 1] = static_cast<u8>(v >> 16);
            table[at + 2] = static_cast<u8>(v >> 8);
            table[at + 3] = static_cast<u8>(v);
        };
        put(0, 0x01000000u);
        put(8, entries);
        for (std::size_t i = 0; i < files.size(); ++i) {
            const std::size_t at = (1u + i) * 12u;
            put(at, name_at[i]);
            put(at + 4, 0xDEADBE00u + static_cast<u32>(i));  // replaced by the base
            put(at + 8, files[i].size);
        }
        std::memcpy(table.data() + static_cast<std::size_t>(entries) * 12u, names.data(),
                    names.size());
        return table;
    }

    // What the file's byte at `offset` must be, or zero past its end.
    u8 expected(std::size_t index, u64 offset) const {
        if (offset >= files[index].size) return 0u;
        return static_cast<u8>((offset * 31u + files[index].name[0]) & 0xFFu);
    }
};

void test_asset_read_contract_and_failures() {
    TestAssetTree tree({{"alpha.dat", 10000}, {"beta.one", 5000}, {"gone.bin", 256, false}});
    gekko::assets::AssetSystem assets;
    std::string error;
    // The namespace comes from the fixture's table so that it can name a file
    // the tree does not hold (a host scan cannot).
    gekko::assets::AssetNamespace names;
    require(gekko::test_support::namespace_from_fst(tree.directory(), names, error),
            "the fixture's table must build a namespace");
    require(shadow::backends::mount_host_directory(assets, tree.root, std::move(names), std::vector<u8>(0x20u, 0x5Au), error),
            "the tree must mount");

    const auto base = [&](const char* name) {
        return assets.space().base_of(assets.registry().find(name));
    };
    std::vector<u8> buffer(16000, 0xAAu);
    u64 got = 0;
    using Status = gekko::assets::AssetSystem::ReadStatus;

    // The whole first asset plus its zero tail - the read every issuer makes,
    // because each rounds its length up to a multiple of 32.
    require(assets.read(base("alpha.dat"), 10016, buffer.data(), got) == Status::Ok,
            "the whole asset plus its tail must read");
    require(got == 10016, "the full requested length must be reported");
    for (u64 i = 0; i < 10016; ++i)
        require(buffer[static_cast<std::size_t>(i)] == tree.expected(0, i),
                "asset bytes then zeroes");

    // An offset read that crosses the end of the second asset.
    require(assets.read(base("beta.one") + 4000, 1024, buffer.data(), got) == Status::Ok,
            "a read crossing the end must succeed");
    for (u64 i = 0; i < 1024; ++i)
        require(buffer[static_cast<std::size_t>(i)] == tree.expected(1, 4000 + i),
                "data then zeroes across the end");

    // CHANGED FROM THE DISC MODEL, and the two refusals are distinct.
    //
    // OUT OF RANGE: still inside the asset's own pages, but past what the
    // guest's own bounds check permits (offset + length <= size + 32). A
    // page-granular test would pass this, because 10,000 bytes of asset own a
    // whole 64 KiB page. On hardware it would have returned the NEXT file's
    // bytes: 2,430 of the real disc's extents are byte-exactly adjacent.
    std::fill(buffer.begin(), buffer.end(), u8{0xAAu});
    require(assets.read(base("alpha.dat") + 10064u, 32, buffer.data(), got) ==
                Status::OutOfRange,
            "an offset past the asset's size is refused, not zero-filled");
    require(assets.read(base("alpha.dat"), 20000, buffer.data(), got) == Status::OutOfRange,
            "a length past the asset's size is refused even inside its page");
    // UNMAPPED: no asset owns the address at all. There is no gap to serve
    // zeroes from and no region below the first asset.
    require(assets.read(base("alpha.dat") + 0x100000u, 512, buffer.data(), got) ==
                Status::Unmapped,
            "an address beyond every asset is unmapped");
    require(assets.read(0x40u, 32, buffer.data(), got) == Status::Unmapped,
            "an address below every asset is unmapped");
    require(buffer[0] == 0xAAu, "a refused read must not touch the destination");

    // Named by the directory, absent from the tree: a failure, not zeroes.
    require(assets.read(base("gone.bin"), 64, buffer.data(), got) == Status::Failed,
            "a missing backing file must fail rather than serve zeroes");

    const gekko::assets::AssetSystem::Stats stats = assets.stats();
    require(stats.unmapped == 2, "both unmapped reads are counted");
    require(stats.out_of_range == 2, "both out-of-range reads are counted");
    require(stats.failed == 1, "the missing-file read is counted");
    require(stats.reads == 2, "and the two good reads are counted");
}

// The DSP only raises its interrupt when a mail is actually queued: the SDK
// handler spins on the mailbox as soon as it is entered.
void test_ax_voice_end_and_loop_windows() {
    const AxVoiceAdvanceResult one_shot =
        advance_ax_voice_position(90u, 100u, 200u, false, 20u);
    require(one_shot.valid && !one_shot.running && !one_shot.looped &&
                one_shot.next == 100u,
            "a one-shot must stop at endAddress even when loopAddress is irrelevant");

    const AxVoiceAdvanceResult intro =
        advance_ax_voice_position(50u, 100u, 80u, true, 10u);
    require(intro.valid && intro.running && !intro.looped && intro.next == 60u,
            "a looping voice may play an intro before loopAddress");

    const AxVoiceAdvanceResult wrapped =
        advance_ax_voice_position(95u, 100u, 80u, true, 10u);
    require(wrapped.valid && wrapped.running && wrapped.looped && wrapped.next == 84u,
            "a looping voice must wrap overshoot inside its inclusive loop span");

    const AxVoiceAdvanceResult invalid =
        advance_ax_voice_position(90u, 100u, 101u, true, 1u);
    require(!invalid.valid,
            "a looping voice with loopAddress above endAddress must be rejected");
}

// ---- native audio (gekko/native_audio.h) ---------------------------------------
//
// There is no ARAM (docs/ARAM.md step 5). What outlives it: the game constructor's ARInit call (a lowered site)
// unmasks the DSP DMA interrupt, and a native sound store's transfer completion raises it and queues its callback.
void test_native_dsp_dma_completion() {
    System system;
    auto& cpu = system.cpu();
    cpu.reset();
    system.start();
    auto& interrupts = system.interrupts();
    using Source = NativeInterrupts::Source;
    interrupts.clear(Source::DspDma);
    cpu.mem_write32(0x800000C4u, 0xFFFFFFFFu);  // OSInit's masks: every global interrupt masked, no user mask
    cpu.mem_write32(0x800000C8u, 0u);
    system.audio().dma_interrupt_init(cpu);
    require(interrupts.unmasked(Source::DspDma), "the ARInit site unmasks the DSP DMA interrupt");
    system.audio().complete_transfer(0x803CF32Cu, 0x80F10000u);
    require(interrupts.status(Source::DspDma), "a native transfer's completion raises the DSP DMA interrupt");
    require(!cpu.stopped(), "the completion stopped the machine");
}

// AI DMA passes run on the game clock: a pass of N 32-byte blocks at 32 kHz
// ends N * 10,125 ticks after it starts, publishes the buffer, raises AIDINT
// and reloads the programmed buffer.
void test_native_ai_pass_timing() {
    System system;
    system.start();
    Cpu& cpu = system.cpu();
    cpu.set_msr(cpu.msr() & (~kMsrEE));
    std::vector<std::pair<u64, u64>> passes;  // bytes, game ticks
    system.audio().set_pass_sink([&](hw::HostAudioPointer64 host, hw::AudioByteOffset64 bytes) {
        require(host != nullptr, "the pass buffer is host-backed MEM1");
        passes.emplace_back(static_cast<u64>(bytes), system.game_ticks());
    });
    const auto call = [&](u32 address, u32 r3, u32 r4) {
        cpu.gpr[3] = r3;
        cpu.gpr[4] = r4;
        cpu.lr = 0x80004000u;
        require(cpu.try_hle(address), "a native audio owner serves every call");
    };
    call(0x80382014u, 0u, 0u);             // AISetDSPSampleRate(32 kHz)
    call(0x80381E74u, 0x80200000u, 0x280u);  // AIInitDMA: 20 blocks
    require(system.advance_game_time(system.game_ticks() + 1000u), "the DMA starts off the pass grid");
    const u64 start = system.game_ticks();
    const u64 offset = cpu.time_base() - start;
    // A 0x280-byte pass at 32 kHz is 20 x 10,125 = 202,500 ticks (5 ms); passes sit on its multiples in the time
    // base (one time base with the 1/60 s frames), so the first one is cut short at the next.
    const u64 first = ((start + offset) / 202500u + 1u) * 202500u - offset;
    call(0x80381EFCu, 0u, 0u);             // AIStartDMA
    require(cpu.game_clock().deadline(GameEvent::Audio) == first && first > start && first - start < 202500u,
            "the first pass ends on the time base's 5 ms pass grid");
    require(system.advance_game_time(first + 202500u), "two passes elapse");
    require(passes.size() == 2u && passes[0] == std::make_pair(u64{0x280u}, first) &&
                passes[1] == std::make_pair(u64{0x280u}, first + 202500u),
            "each pass is published at its own end and the engine reloads the buffer without a gap");
    require(system.interrupts().status(NativeInterrupts::Source::AiDma),
            "a pass end raises AIDINT");
    call(0x80381F14u, 0u, 0u);  // AIStopDMA
    require(cpu.game_clock().deadline(GameEvent::Audio) == kNoDeadline,
            "a stopped engine schedules nothing");
}

// The native interrupt line follows status AND mask: SetInterruptMask applies
// the DSP group (OS interrupts 5-7) to the native sources.
void test_native_interrupt_line_follows_status_and_mask() {
    System system;
    system.start();
    Cpu& cpu = system.cpu();
    NativeInterrupts& interrupts = system.interrupts();
    using Source = NativeInterrupts::Source;
    interrupts.raise(Source::DspDma);
    require(interrupts.status(Source::DspDma) && !cpu.native_interrupt(),
            "a masked source is pending but must not assert the line");
    cpu.gpr[3] = 0x07000000u;  // the DSP group
    cpu.gpr[4] = ~0x02000000u;  // only DSP_ARAM (6) enabled
    cpu.lr = 0x80004000u;
    require(cpu.try_hle(0x8037620Cu), "SetInterruptMask is served natively");
    require(cpu.gpr[3] == 0u, "the DSP group is applied and removed from the mask");
    require(interrupts.unmasked(Source::DspDma) && !interrupts.unmasked(Source::AiDma) &&
                !interrupts.unmasked(Source::Dsp),
            "only the enabled source is unmasked");
    require(cpu.native_interrupt(), "an unmasked pending source asserts the native line");
    interrupts.clear(Source::DspDma);
    require(!cpu.native_interrupt(), "clearing the source lowers the line");
    interrupts.raise(Source::AiDma);
    require(!cpu.native_interrupt(), "a masked source keeps the line low");
}

// The ARAM DMA engine copies through Bus::read_block/write_block rather than a
// per-byte Bus decode. The only discontinuity in either direction is the ARAM
// ring wrap, so that is what has to be proven: a transfer straddling the end of
// the 16 MiB ring must place exactly the same bytes the byte-at-a-time engine
// did, in both directions, and must not disturb the guest-visible counters.
// The graphics pipe has to decode the command stream the write-gather pipe
// carries, including BP register writes and primitive payloads.
void test_system_load_dol_preserves_data_inside_bss_header() {
    System system;
    DolProgram program;
    program.bytes = {0x40, 0x49, 0x0f, 0xdb};
    program.sections.push_back(DolSectionInfo{0, 0x80005000u, 4u, false});
    program.bss_address = 0x80004000u;
    program.bss_size = 0x2000u;
    program.entry_point = 0x80005000u;
    system.load_dol(program);
    require(system.guest_memory().read32(0x80005000u) == 0x40490fdbu,
            "System::load_dol must keep initialised data that sits inside the BSS header");
    require(system.guest_memory().read32(0x80004000u) == 0u, "BSS below that data must still be zero");
}


















// ---- stage 5: the memory card and the IPL services --------------------------
// docs/native-time-20261005/STAGE5_EXI.md. The EXI device model's protocol
// tests (channel status, the chip's command bytes, NAND programming, DMA
// registers) went with the device; what the guest observes is tested at the
// owners it calls, and the host file policy - unchanged - at the native store.
namespace {

constexpr u32 kTestSda = 0x805EC500u;
constexpr u32 kCard0 = 0x805A2CA0u;  // __CARDBlock[0]
constexpr u32 kScbAddress = 0x80586FE0u;
constexpr u32 kCardEraseSectorAddress = 0x8038DD2Cu;
constexpr u32 kCardReadAddress = 0x8038F56Cu;
constexpr u32 kCardWriteAddress = 0x8038F6B8u;
constexpr u32 kCardProbeExAddress = 0x80390D90u;
constexpr u32 kCardMountAsyncAddress = 0x80391498u;
constexpr u32 kCardUnmountAddress = 0x803916D4u;
constexpr u32 kInitSramAddress = 0x803776F4u;
constexpr u32 kUnlockSramAddress = 0x803778E8u;
constexpr u32 kReadRomAddress = 0x80377C7Cu;
constexpr u32 kInitMetroTrkAddress = 0x8039F8E0u;
constexpr u32 kInitMetroTrkBbaAddress = 0x8039F978u;
constexpr u32 kExiInitAddress = 0x803B4CFCu;
constexpr u32 kWriteUartNAddress = 0x803B5848u;
constexpr u32 kCardResultBusy = 0xFFFFFFFFu;     // -1
constexpr u32 kCardResultNoCard = 0xFFFFFFFDu;   // -3
constexpr u32 kCardResultIoError = 0xFFFFFFFBu;  // -5
constexpr u32 kCardResultFatal = 0xFFFFFF80u;    // -128

u32 call_owner(Cpu& cpu, u32 address, std::initializer_list<u32> args) {
    u32 r = 3u;
    for (const u32 value : args) cpu.gpr[r++] = value;
    cpu.lr = 0x81004000u;
    require(cpu.try_hle(address), "a native owner serves every call");
    return cpu.gpr[3];
}

// The CARD library's SectorSizeTable and LatencyTable, as the DOL holds them.
void seed_card_tables(Cpu& cpu) {
    const u32 sectors[] = {0x2000u, 0x4000u, 0x8000u, 0x10000u, 0x20000u, 0x40000u, 0u, 0u};
    const u32 latencies[] = {4u, 8u, 16u, 32u, 64u, 128u, 256u, 512u};
    for (u32 i = 0; i < 8u; ++i) {
        cpu.mem_write32(0x805684E0u + 4u * i, sectors[i]);
        cpu.mem_write32(0x80568500u + 4u * i, latencies[i]);
    }
}

void card_checksum_into(std::vector<u8>& raw, std::size_t data, std::size_t length, std::size_t stored) {
    u16 checksum = 0u;
    u16 inverse = 0u;
    for (std::size_t offset = 0; offset < length; offset += 2u) {
        const u16 word = load_be16(raw.data() + data + offset);
        checksum = static_cast<u16>(checksum + word);
        inverse = static_cast<u16>(inverse + static_cast<u16>(~word));
    }
    store_be16(raw.data() + stored, checksum == 0xffffu ? 0u : checksum);
    store_be16(raw.data() + stored + 2u, inverse == 0xffffu ? 0u : inverse);
}

// The test card header: decodes through the IPL algorithm to card-A flash ID
// "DOLPHINSLOTA", checksum 0x6e.
constexpr std::array<u8, 20> kTestCardHeader{{0xa0, 0xa9, 0x4c, 0x07, 0xa5, 0x67, 0xe2, 0x34, 0xb8, 0x16,
                                              0xf2, 0xac, 0x00, 0x00, 0x00, 0x00, 0x2b, 0xf5, 0x23, 0x91}};

// A valid raw card of `blocks` 8 KiB blocks with an empty file system: the
// second directory and allocation-table copies are the current ones.
std::vector<u8> make_test_card(std::size_t blocks) {
    constexpr std::size_t kBlockSize = 8192u;
    std::vector<u8> raw(blocks * kBlockSize, 0xffu);
    std::copy(kTestCardHeader.begin(), kTestCardHeader.end(), raw.begin());
    store_be16(raw.data() + 0x4000u + 0x1ffau, 3u);
    std::fill(raw.begin() + 0x8000, raw.begin() + 0xA000, u8{0});
    store_be16(raw.data() + 0x8000u + 4u, 3u);
    store_be16(raw.data() + 0x8000u + 6u, static_cast<u16>(blocks - 5u));
    card_checksum_into(raw, 0u, 0x1fcu, 0x1fcu);
    card_checksum_into(raw, 0x4000u, 0x1ffcu, 0x5ffcu);
    card_checksum_into(raw, 0x8004u, 0x1ffcu, 0x8000u);
    return raw;
}

void write_test_file(const std::filesystem::path& path, const std::vector<u8>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "test memory-card fixture must be writable");
}

std::vector<u8> read_test_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(static_cast<bool>(input), "test memory-card output must be readable");
    const std::streamoff size = input.tellg();
    std::vector<u8> bytes(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    require(static_cast<bool>(input), "test memory-card output must be complete");
    return bytes;
}

std::filesystem::path fresh_test_directory(const char* stem) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        (std::string(stem) + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    return directory;
}

// CARDControl[0] as DoMount leaves a mounted 16 MiB-geometry card.
void seed_mounted_card(Cpu& cpu) {
    cpu.mem_write32(kCard0 + 0x00u, 1u);       // attached
    cpu.mem_write32(kCard0 + 0x04u, 0u);       // result READY
    cpu.mem_write16(kCard0 + 0x0Au, 0x80u);    // pageSize
    cpu.mem_write32(kCard0 + 0x0Cu, 0x2000u);  // sectorSize
    cpu.mem_write32(kCard0 + 0x24u, 7u);       // mountStep
    cpu.mem_write32(kCard0 + 0x10Cu, 0x80000000u);  // diskID
}

// The completion a native operation posted, taken as its delivery takes it
// (the delivery itself runs guest callbacks: the reference route covers it).
void take_card_completion(System& system) {
    require(system.interrupts().status(NativeInterrupts::Source::Card), "the operation posted its completion");
    system.card().reset();
    system.interrupts().clear(NativeInterrupts::Source::Card);
}

}  // namespace

// The host store's policy is the emulated device's, unchanged: a GCI is a real
// save-file payload copied into the allocation chain already recorded by a raw
// base card, kept in a per-run working image bound to that GCI by a marker;
// the SRAM companion and the card's derived flash identity; a corrupt working
// card is rebuilt, a foreign or broken one refused. The guest reaches the bytes
// only through the CARD library's block-device owners.
void test_native_card_store_policy() {
    constexpr std::size_t kBlockSize = 8192u;
    const std::filesystem::path directory = fresh_test_directory("gekko-memory-card-");
    const std::filesystem::path gci_path = directory / "8P-GUPP-SHADOWTHEHEDGEHOG.gci";
    const std::filesystem::path raw_path = directory / "Memory Card A.raw";
    const std::filesystem::path sram_path = directory / "SRAM.raw";
    const std::filesystem::path working_path = directory / "8P-GUPP-SHADOWTHEHEDGEHOG.pc.raw";

    std::vector<u8> gci(64u + 2u * kBlockSize, 0u);
    std::memcpy(gci.data(), "GUPP8P", 6u);
    std::memcpy(gci.data() + 8u, "SHADOWTHEHEDGEHOG", 19u);
    store_be16(gci.data() + 0x38u, 2u);
    for (std::size_t index = 64u; index < gci.size(); ++index) gci[index] = static_cast<u8>((index - 64u) ^ 0x5au);

    // 4 Mbit, the smallest card IsCard accepts.
    std::vector<u8> raw(64u * kBlockSize, 0xffu);
    std::copy(kTestCardHeader.begin(), kTestCardHeader.end(), raw.begin());
    std::copy_n(gci.data(), 64u, raw.data() + 0x4000u);
    store_be16(raw.data() + 0x4000u + 0x36u, 6u);
    store_be16(raw.data() + 0x4000u + 0x38u, 2u);
    store_be16(raw.data() + 0x4000u + 0x1ffau, 3u);
    store_be16(raw.data() + 0x8000u + 4u, 3u);
    store_be16(raw.data() + 0x8000u + 0x0cu, 7u);       // block 6 -> block 7
    store_be16(raw.data() + 0x8000u + 0x0eu, 0xffffu);  // block 7 -> end
    card_checksum_into(raw, 0u, 0x1fcu, 0x1fcu);
    card_checksum_into(raw, 0x4000u, 0x1ffcu, 0x5ffcu);
    card_checksum_into(raw, 0x8004u, 0x1ffcu, 0x8000u);
    write_test_file(gci_path, gci);
    write_test_file(raw_path, raw);
    std::vector<u8> sram(0x44u, 0u);
    // The fixture header decodes to this exact card-A identity / checksum
    // pair; a companion from another card must not be accepted merely
    // because it is 68 bytes long.
    std::memcpy(sram.data() + 0x18u, "DOLPHINSLOTA", 12u);
    sram[0x3eu] = 0x6eu;
    write_test_file(sram_path, sram);

    std::vector<u8> pattern(kBlockSize);
    for (std::size_t i = 0; i < pattern.size(); ++i) pattern[i] = static_cast<u8>(i * 7u + 3u);
    {
        System system;
        system.start();
        system.set_memory_card_path(gci_path);
        require(system.memory_card_present(), "a valid GCI must attach card A");
        require(system.memory_card_backing_path() == working_path, "GCI writes must use the separate PC raw card");
        require(std::filesystem::file_size(working_path.string() + ".source") == 32u,
                "a GCI working card must carry an exact source-binding marker");
        require(system.memory_card_sram_path() == sram_path,
                "the card must use its real 68-byte SRAM companion when available");
        require(std::memcmp(system.ipl().settings().data() + 0x18u, "DOLPHINSLOTA", 12u) == 0 &&
                    system.ipl().settings()[0x3eu] == 0x6eu,
                "the settings must carry the companion's card A flash identity");
        std::vector<u8> working = read_test_file(working_path);
        require(std::equal(gci.begin() + 64, gci.begin() + 64 + kBlockSize, working.begin() + 6u * kBlockSize),
                "GCI block zero must follow the raw card's first allocated block");
        require(std::equal(gci.begin() + 64 + kBlockSize, gci.end(), working.begin() + 7u * kBlockSize),
                "GCI block one must follow the raw card BAT chain");
        require(read_test_file(raw_path)[6u * kBlockSize] == 0xffu, "import must not modify the source raw card");
        require(system.card().image() == working, "the attached card is the working image");

        Cpu& cpu = system.cpu();
        cpu.gpr[13] = kTestSda;
        seed_mounted_card(cpu);
        // __CARDRead of block 6 into a 32-byte aligned buffer: the bytes at
        // once, the transfer fields as the last segment left them.
        require(call_owner(cpu, kCardReadAddress, {0u, 6u * kBlockSize, 0x2000u, 0x81001000u, 0x817F0000u}) == 0u,
                "__CARDRead of a mounted card is READY");
        bool same = true;
        for (u32 i = 0; i < kBlockSize; ++i) same = same && cpu.mem_read8(0x81001000u + i) == gci[64u + i];
        require(same, "a card read returns the imported allocation's bytes");
        require(cpu.mem_read32(kCard0 + 0xACu) == 0u && cpu.mem_read32(kCard0 + 0xB0u) == 7u * kBlockSize &&
                    cpu.mem_read32(kCard0 + 0xB4u) == 0x81003000u && cpu.mem_read32(kCard0 + 0xB8u) == 0x2000u &&
                    cpu.mem_read32(kCard0 + 0xD4u) == 0x817F0000u,
                "a read leaves repeat, addr, buffer, xferred and its transfer callback as BlockReadCallback did");
        take_card_completion(system);

        // The file system erases a sector before it writes it.
        require(call_owner(cpu, kCardEraseSectorAddress, {0u, 6u * kBlockSize + 0x200u, 0x817F0010u}) == 0u,
                "__CARDEraseSector of a mounted card is READY");
        require(std::all_of(system.card().image().begin() + 6 * kBlockSize,
                            system.card().image().begin() + 7 * kBlockSize, [](u8 b) { return b == 0xffu; }),
                "an erase fills the 8 KiB sector holding the address with 0xFF");
        require(cpu.mem_read32(kCard0 + 0xCCu) == 0x817F0010u, "the erase keeps its callback where __CARDStart did");
        take_card_completion(system);
        for (u32 i = 0; i < kBlockSize; ++i) cpu.mem_write8(0x81004000u + i, pattern[i]);
        require(!system.card().persist_tick_wired(), "no tick has reached this card yet");
        const u64 persists_before = system.card().stats().persists;
        require(call_owner(cpu, kCardWriteAddress, {0u, 6u * kBlockSize, 0x2000u, 0x81004000u, 0x817F0020u}) == 0u,
                "__CARDWrite of a mounted card is READY");
        require(std::equal(pattern.begin(), pattern.end(), system.card().image().begin() + 6 * kBlockSize),
                "a write stores its bytes");
        require(system.card().stats().overwritten_bytes == 0u, "the write landed on erased bytes");
        require(cpu.mem_read32(kCard0 + 0xB0u) == 7u * kBlockSize && cpu.mem_read32(kCard0 + 0xB4u) == 0x81006000u &&
                    cpu.mem_read32(kCard0 + 0xD4u) == 0x817F0020u && cpu.mem_read32(kCard0 + 0xCCu) == 0u,
                "a write leaves the transfer fields as BlockWriteCallback did");
        require(system.card().stats().persists == persists_before + 1u && !system.card().dirty(),
                "without a delivered tick, a card write persists at once");
        working = read_test_file(working_path);
        require(std::equal(pattern.begin(), pattern.end(), working.begin() + 6u * kBlockSize),
                "the write persists to the working card");
        require(read_test_file(raw_path)[6u * kBlockSize] == 0xffu,
                "card writes must never leak back to the source raw used by a GCI import");
        take_card_completion(system);

        // The debounce: once a tick proves the drain wired, writes only dirty
        // the image and one durable write lands after 250 ms of game time.
        system.card().tick(4096u);
        require(system.card().persist_tick_wired(), "one tick proves the drain hook is wired");
        const u64 deferred_before = system.card().stats().persists_deferred;
        for (u32 i = 0; i < 4u; ++i) {
            system.card().tick(4096u);
            for (u32 j = 0; j < 0x80u; ++j) cpu.mem_write8(0x81004000u + j, static_cast<u8>(0xb0u + i));
            require(call_owner(cpu, kCardWriteAddress, {0u, 7u * kBlockSize + 0x80u * i, 0x80u, 0x81004000u, 0u}) == 0u,
                    "a one-page write is READY");
            take_card_completion(system);
        }
        require(system.card().stats().persists == persists_before + 1u,
                "with the tick wired, card writes must not persist at once");
        require(system.card().stats().persists_deferred == deferred_before + 4u, "every absorbed write is counted");
        require(system.card().dirty(), "the written bytes are pending in the image");
        require(read_test_file(working_path)[7u * kBlockSize] != 0xb0u,
                "the host file must not have been rewritten by the deferred writes yet");
        require(call_owner(cpu, kCardReadAddress, {0u, 7u * kBlockSize, 0x200u, 0x81008000u, 0u}) == 0u &&
                    cpu.mem_read8(0x81008000u) == 0xb0u && cpu.mem_read8(0x81008080u) == 0xb1u &&
                    cpu.mem_read8(0x81008180u) == 0xb3u,
                "deferred host persistence must not change what the guest reads back");
        take_card_completion(system);
        system.card().tick(486000000ull / 4u - 1u);
        require(system.card().stats().persists == persists_before + 1u,
                "one cycle short of the quiet interval must not persist");
        system.card().tick(1u);
        require(system.card().stats().persists == persists_before + 2u && !system.card().dirty(),
                "the quiet interval elapsing must persist exactly once");
        working = read_test_file(working_path);
        require(working[7u * kBlockSize] == 0xb0u && working[7u * kBlockSize + 0x180u] == 0xb3u,
                "the single deferred write must carry every written page");
        system.card().flush();
        require(system.card().stats().persists == persists_before + 2u, "flushing a clean image must not write again");
    }

    // Guest-written progress survives a re-attach of the same GCI: the
    // working image is bound by the marker and not re-imported.
    {
        System reload;
        reload.set_memory_card_path(gci_path);
        require(std::equal(pattern.begin(), pattern.end(), reload.card().image().begin() + 6 * kBlockSize),
                "re-attaching the GCI must keep guest-written card bytes");
    }

    // CARDWrite of an existing file bumps the directory counter and leaves the
    // BAT generation unchanged. Re-attach must still accept the card.
    std::vector<u8> working = read_test_file(working_path);
    store_be16(working.data() + 0x4000u + 0x1ffau, 4u);
    card_checksum_into(working, 0x4000u, 0x1ffcu, 0x5ffcu);
    write_test_file(working_path, working);
    {
        System pair;
        pair.set_memory_card_path(gci_path);
        require(pair.memory_card_present(), "a valid directory/BAT pair with unequal update counters must still mount");
    }

    // A checksum-invalid working card bound to this GCI is rebuilt from the
    // source payload instead of aborting the boot - after it is kept: it may
    // hold in-game progress newer than the GCI.
    working = read_test_file(working_path);
    working[0x4000u] ^= 1u;
    write_test_file(working_path, working);
    {
        System rebuilt;
        rebuilt.set_memory_card_path(gci_path);
        require(std::equal(gci.begin() + 64, gci.begin() + 64 + kBlockSize,
                           rebuilt.card().image().begin() + 6 * kBlockSize),
                "a corrupt working card must be re-imported from the source GCI");
        const std::filesystem::path kept = working_path.string() + ".unusable";
        require(std::filesystem::exists(kept) && read_test_file(kept) == working,
                "the corrupt working card must be kept byte for byte before the rebuild replaces it");
    }
    working = read_test_file(working_path);
    working[0x4000u] ^= 1u;
    write_test_file(working_path, working);
    {
        System rebuilt_again;
        rebuilt_again.set_memory_card_path(gci_path);
        require(read_test_file(working_path.string() + ".unusable.1") == working,
                "a second unusable working card must not overwrite the first kept copy");
    }

    // A real SRAM belonging to another card is no companion (the mount would
    // reject the card): the IPL's card-header derivation is used instead, as
    // for a card with no SRAM beside it.
    write_test_file(sram_path, std::vector<u8>(0x44u, 0u));
    {
        System mismatch;
        mismatch.set_memory_card_path(gci_path);
        require(mismatch.memory_card_sram_path().empty(), "a mismatched SRAM companion must not be attached to card A");
        require(std::memcmp(mismatch.ipl().settings().data() + 0x18u, "DOLPHINSLOTA", 12u) == 0 &&
                    mismatch.ipl().settings()[0x3eu] == 0x6eu,
                "mismatched SRAM fallback must derive the card's flash ID");
    }
    std::filesystem::remove(sram_path);
    {
        System derived;
        derived.set_memory_card_path(gci_path);
        require(derived.memory_card_sram_path().empty(), "a card without SRAM must report that its identity was derived");
        require(std::memcmp(derived.ipl().settings().data() + 0x18u, "DOLPHINSLOTA", 12u) == 0 &&
                    derived.ipl().settings()[0x3eu] == 0x6eu,
                "the derived card flash ID must be DOLPHINSLOTA with checksum 0x6e");
    }

    std::vector<u8> wrong_marker = read_test_file(working_path.string() + ".source");
    wrong_marker.back() ^= 0x80u;
    write_test_file(working_path.string() + ".source", wrong_marker);
    bool rejected_wrong_source = false;
    try {
        System rejected;
        rejected.set_memory_card_path(gci_path);
    } catch (const std::runtime_error&) {
        rejected_wrong_source = true;
    }
    require(rejected_wrong_source, "a working card bound to another GCI source must be rejected");

    const std::filesystem::path corrupt_path = directory / "corrupt.raw";
    std::vector<u8> corrupt = raw;
    corrupt[0x4000u] ^= 1u;
    write_test_file(corrupt_path, corrupt);
    bool rejected_bad_metadata = false;
    try {
        System rejected;
        rejected.set_memory_card_path(corrupt_path);
    } catch (const std::runtime_error&) {
        rejected_bad_metadata = true;
    }
    require(rejected_bad_metadata, "a raw card with no checksum-valid directory/BAT generation must be rejected");

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// Cards the host store creates, and the sizes it refuses. A card path that
// does not exist yet (a first run with no imported save) is created formatted
// and empty, as __CARDFormatRegionAsync leaves a card, and mounts through the
// owners; a GCI with no base card recording its allocation is placed on a new
// card as CARDCreate + CARDWrite would place it; a size IsCard refuses is
// refused at attach instead of attaching a card every mount fails WRONGDEVICE.
void test_native_card_created_cards() {
    constexpr std::size_t kBlockSize = 8192u;
    const std::filesystem::path directory = fresh_test_directory("gekko-card-created-");
    const auto checksum_ok = [](const std::vector<u8>& image, std::size_t data, std::size_t length,
                                std::size_t stored) {
        std::vector<u8> copy = image;
        card_checksum_into(copy, data, length, stored);
        return std::equal(copy.begin() + static_cast<std::ptrdiff_t>(stored),
                          copy.begin() + static_cast<std::ptrdiff_t>(stored + 4u),
                          image.begin() + static_cast<std::ptrdiff_t>(stored));
    };
    const auto all_bytes = [](const std::vector<u8>& image, std::size_t from, std::size_t to, u8 value) {
        return std::all_of(image.begin() + static_cast<std::ptrdiff_t>(from),
                           image.begin() + static_cast<std::ptrdiff_t>(to), [value](u8 b) { return b == value; });
    };

    // The missing card: created in its (missing) directory and attached.
    const std::filesystem::path created_path = directory / "save-data" / "ShadowTheHedgehog.pc.raw";
    {
        System system;
        system.start();
        system.set_memory_card_path(created_path);
        require(system.memory_card_present() && std::filesystem::exists(created_path),
                "a missing .raw card must be created and attached");
        const std::vector<u8> image = read_test_file(created_path);
        require(image.size() == 64u * kBlockSize && system.card().image() == image,
                "a created card is a 4-Mbit image, on disk before the guest uses it");
        require(load_be16(image.data() + 0x20u) == 0u && load_be16(image.data() + 0x22u) == 4u &&
                    load_be16(image.data() + 0x24u) == 0u && checksum_ok(image, 0u, 0x1fcu, 0x1fcu),
                "the created header: device 0, 4 Mbit, ANSI encoding, a valid checksum pair");
        require(all_bytes(image, 0x200u, kBlockSize, 0xffu), "the header block past the ID is erased");
        for (std::size_t copy = 0; copy < 2u; ++copy) {
            const std::size_t dir = (1u + copy) * kBlockSize;
            const std::size_t bat = (3u + copy) * kBlockSize;
            require(all_bytes(image, dir, dir + 0x1ffau, 0xffu), "every directory entry of a created card is free");
            require(load_be16(image.data() + dir + 0x1ffau) == copy && checksum_ok(image, dir, 0x1ffcu, dir + 0x1ffcu),
                    "each directory copy carries its check code and a valid checksum pair");
            require(load_be16(image.data() + bat + 4u) == copy && load_be16(image.data() + bat + 6u) == 59u &&
                        load_be16(image.data() + bat + 8u) == 4u && checksum_ok(image, bat + 4u, 0x1ffcu, bat),
                    "each allocation table: check code, 59 free blocks, last slot 4, a valid checksum pair");
            require(all_bytes(image, bat + 0x0au, bat + kBlockSize, 0u), "every user block of a created card is free");
        }
        require(all_bytes(image, 5u * kBlockSize, image.size(), 0xffu), "the user blocks of a created card are erased");
        require(std::memcmp(system.ipl().settings().data() + 0x18u, "SHADOWPCSAVE", 12u) == 0,
                "the IPL derives the flash ID the card was formatted against");

        // The mount's identity and SRAM flash-ID checks accept it.
        Cpu& cpu = system.cpu();
        cpu.gpr[13] = kTestSda;
        seed_card_tables(cpu);
        cpu.mem_write32(kCard0 + 0x10Cu, 0x80000000u);
        cpu.mem_write32(kCard0 + 0x110u + 0x10Cu, 0x80000000u);
        call_owner(cpu, kInitSramAddress, {});
        require(call_owner(cpu, kCardProbeExAddress, {0u, 0x81003000u, 0x81003004u}) == 0u &&
                    cpu.mem_read32(0x81003000u) == 4u && cpu.mem_read32(0x81003004u) == 0x2000u,
                "CARDProbeEx finds a 4-Mbit card of 8 KiB sectors in slot A");
        const u32 work = 0x81010000u;
        require(call_owner(cpu, kCardMountAsyncAddress, {0u, work, 0u, 0u}) == 0u &&
                    cpu.mem_read16(kCard0 + 0x10u) == 64u,
                "CARDMountAsync of a created card passes DoMount's flash-ID check (64 blocks)");
        bool system_blocks = true;
        for (u32 i = 0; i < 0xA000u; ++i) system_blocks = system_blocks && cpu.mem_read8(work + i) == image[i];
        require(system_blocks, "the created card's five system blocks reach the work area");
        take_card_completion(system);
        require(!cpu.stopped(), "nothing above stops the machine");
    }
    {
        std::vector<u8> image = read_test_file(created_path);
        image[5u * kBlockSize] = 0x42u;
        write_test_file(created_path, image);
        System again;
        again.set_memory_card_path(created_path);
        require(again.card().image()[5u * kBlockSize] == 0x42u, "an existing card is attached, never formatted again");
    }

    // A GCI with no base card beside it, and one whose base card records no
    // allocation for it: both are placed on a new card.
    std::vector<u8> gci(64u + 3u * kBlockSize, 0u);
    std::memcpy(gci.data(), "GUPP8P", 6u);
    std::memcpy(gci.data() + 8u, "SHADOWTHEHEDGEHOG", 17u);
    store_be16(gci.data() + 0x36u, 0x0123u);  // its block on the card it came from
    store_be16(gci.data() + 0x38u, 3u);
    for (std::size_t index = 64u; index < gci.size(); ++index) gci[index] = static_cast<u8>((index - 64u) * 13u + 1u);
    for (const bool with_base : {false, true}) {
        const std::filesystem::path folder = directory / (with_base ? "unrelated-base" : "lone");
        std::filesystem::create_directories(folder);
        const std::filesystem::path gci_path = folder / "8P-GUPP-SHADOWTHEHEDGEHOG.gci";
        const std::filesystem::path working_path = folder / "working.pc.raw";
        write_test_file(gci_path, gci);
        const std::vector<u8> base = make_test_card(64u);
        if (with_base) write_test_file(folder / "Memory Card A.raw", base);
        System system;
        system.set_memory_card_path(gci_path, working_path);
        require(system.memory_card_present() && system.memory_card_backing_path() == working_path,
                "a GCI without a base card holding its allocation must still attach");
        const std::vector<u8> image = read_test_file(working_path);
        require(system.card().image() == image && image.size() == 64u * kBlockSize,
                "the GCI's new card is a 4-Mbit working image");
        for (std::size_t copy = 0; copy < 2u; ++copy) {
            const std::size_t dir = (1u + copy) * kBlockSize;
            const std::size_t bat = (3u + copy) * kBlockSize;
            require(std::equal(gci.begin(), gci.begin() + 0x36, image.begin() + static_cast<std::ptrdiff_t>(dir)) &&
                        load_be16(image.data() + dir + 0x36u) == 5u &&
                        std::equal(gci.begin() + 0x38, gci.begin() + 64,
                                   image.begin() + static_cast<std::ptrdiff_t>(dir + 0x38u)) &&
                        all_bytes(image, dir + 64u, dir + 0x1ffau, 0xffu) &&
                        checksum_ok(image, dir, 0x1ffcu, dir + 0x1ffcu),
                    "the GCI header is the only directory entry, starting at the first user block");
            require(load_be16(image.data() + bat + 0x0au) == 6u && load_be16(image.data() + bat + 0x0cu) == 7u &&
                        load_be16(image.data() + bat + 0x0eu) == 0xffffu &&
                        all_bytes(image, bat + 0x10u, bat + kBlockSize, 0u) &&
                        load_be16(image.data() + bat + 6u) == 56u && load_be16(image.data() + bat + 8u) == 7u &&
                        checksum_ok(image, bat + 4u, 0x1ffcu, bat),
                    "the allocation table chains blocks 5-6-7 and counts 56 free blocks");
        }
        require(std::equal(gci.begin() + 64, gci.end(), image.begin() + 5 * static_cast<std::ptrdiff_t>(kBlockSize)),
                "the GCI payload fills its allocated blocks");
        require(read_test_file(working_path.string() + ".source").size() == 32u,
                "the new working card is bound to its GCI");
        if (with_base) {
            require(read_test_file(folder / "Memory Card A.raw") == base, "an unrelated base card is never modified");
        }
    }

    // Sizes: IsCard accepts 4 to 128 Mbit.
    for (const std::size_t blocks : {16u, 32u, 4096u}) {
        const std::filesystem::path path = directory / ("refused-" + std::to_string(blocks) + ".raw");
        write_test_file(path, make_test_card(blocks));
        bool refused = false;
        try {
            System system;
            system.set_memory_card_path(path);
        } catch (const std::runtime_error&) {
            refused = true;
        }
        require(refused, "a card size IsCard refuses must be refused at attach");
    }
    {
        const std::filesystem::path path = directory / "largest.raw";
        write_test_file(path, make_test_card(2048u));
        System system;
        system.set_memory_card_path(path);
        require(system.memory_card_present(), "a 16 MiB (128 Mbit) card must attach");
    }

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// CARDProbeEx, CARDMountAsync and CARDUnmount over the native card: the
// identity the emulated card presented, DoMount's steps 0-1 at once (the
// unlocked path's SRAM flash-ID check through the OS SRAM block) and the five
// system blocks read into the work area, the completion posted; the error
// paths take DoUnmount's.
void test_native_card_mount_probe_unmount() {
    // __CARDDefaultApiCallback's address is the game's (a blr the CARD library compiled in); the model takes it from
    // its caller, so a stand-in address serves here and the CARD library body is not pinned by this file.
    constexpr u32 kTestDefaultApiCallback = 0x81004100u;
    const std::filesystem::path directory = fresh_test_directory("gekko-card-mount-");
    const std::filesystem::path card_path = directory / "card.raw";
    const std::vector<u8> raw = make_test_card(64u);  // 512 KiB: 4 Mbit
    write_test_file(card_path, raw);
    {
        System system;
        system.start();
        system.set_memory_card_path(card_path);
        system.card().set_default_api_callback(kTestDefaultApiCallback);
        Cpu& cpu = system.cpu();
        cpu.gpr[13] = kTestSda;
        seed_card_tables(cpu);
        cpu.mem_write32(kCard0 + 0x10Cu, 0x80000000u);           // CARDInit's diskID
        cpu.mem_write32(kCard0 + 0x110u + 0x10Cu, 0x80000000u);
        // __OSInitSram: the settings (with the card's derived flash ID) in the
        // OS's SRAM block, as the boot leaves it before any mount.
        call_owner(cpu, kInitSramAddress, {});
        require(std::memcmp(system.ipl().settings().data() + 0x18u, "DOLPHINSLOTA", 12u) == 0,
                "the settings carry the card's flash ID");

        require(call_owner(cpu, kCardProbeExAddress, {0u, 0x81003000u, 0x81003004u}) == 0u &&
                    cpu.mem_read32(0x81003000u) == 4u && cpu.mem_read32(0x81003004u) == 0x2000u,
                "CARDProbeEx reads the card's size (4 Mbit) and sector size from its ID word");
        require(call_owner(cpu, kCardProbeExAddress, {1u, 0u, 0u}) == kCardResultNoCard, "slot B is empty");
        require(call_owner(cpu, kCardProbeExAddress, {2u, 0u, 0u}) == kCardResultFatal, "a third channel is fatal");

        const u32 work = 0x81010000u;
        require(call_owner(cpu, kCardMountAsyncAddress, {0u, work, 0u, 0u}) == 0u, "the mount starts");
        require(cpu.mem_read32(kCard0) == 1u && cpu.mem_read32(kCard0 + 4u) == kCardResultBusy &&
                    cpu.mem_read32(kCard0 + 0x24u) == 2u,
                "a started mount is attached and BUSY at step 2 until its completion");
        require(cpu.mem_read32(kCard0 + 0x108u) == 4u && cpu.mem_read16(kCard0 + 8u) == 4u &&
                    cpu.mem_read16(kCard0 + 0x0Au) == 0x80u && cpu.mem_read32(kCard0 + 0x0Cu) == 0x2000u &&
                    cpu.mem_read16(kCard0 + 0x10u) == 64u && cpu.mem_read16(kCard0 + 0x12u) == 0xC221u &&
                    cpu.mem_read32(kCard0 + 0x14u) == 4u,
                "DoMount's identity: cid, size, page size, sector size, blocks, vendor 0xC221, latency");
        require(cpu.mem_read32(kCard0 + 0x80u) == work && cpu.mem_read32(kCard0 + 0xD0u) == kTestDefaultApiCallback &&
                    cpu.mem_read32(kCard0 + 0xCCu) == 0u && cpu.mem_read32(kCard0 + 0xDCu) == 0u &&
                    cpu.mem_read32(kCard0 + 0x84u) == 0u && cpu.mem_read32(kCard0 + 0x88u) == 0u,
                "CARDMountAsync's fields: the work area, the default API callback, cleared callbacks and copies");
        bool system_blocks = true;
        for (u32 i = 0; i < 0xA000u; ++i) system_blocks = system_blocks && cpu.mem_read8(work + i) == raw[i];
        require(system_blocks, "the five system blocks are in the work area");
        require(cpu.mem_read32(kCard0 + 0xB8u) == 0xA000u && cpu.mem_read32(kCard0 + 0xB0u) == 0xA000u &&
                    cpu.mem_read32(kCard0 + 0xB4u) == work + 0xA000u && cpu.mem_read32(kCard0 + 0xACu) == 0u,
                "the system-block reads leave the transfer fields as __CARDRead did");
        require(call_owner(cpu, kCardProbeExAddress, {0u, 0x81003000u, 0u}) == 0u && cpu.mem_read32(0x81003000u) == 4u,
                "CARDProbeEx of an attached card reads the control block (mountStep >= 1)");
        require(call_owner(cpu, kCardMountAsyncAddress, {0u, work, 0u, 0u}) == kCardResultBusy,
                "a second mount while the first is BUSY is refused BUSY");
        require(call_owner(cpu, kCardUnmountAddress, {0u}) == kCardResultBusy, "CARDUnmount of a BUSY card is BUSY");
        take_card_completion(system);
        cpu.mem_write32(kCard0 + 4u, 0u);  // what the mount's completion put (READY)

        require(call_owner(cpu, kCardUnmountAddress, {0u}) == 0u && cpu.mem_read32(kCard0) == 0u &&
                    cpu.mem_read32(kCard0 + 4u) == kCardResultNoCard && cpu.mem_read32(kCard0 + 0x24u) == 0u,
                "CARDUnmount detaches: not attached, NOCARD, step 0");
        require(call_owner(cpu, kCardUnmountAddress, {0u}) == kCardResultNoCard, "an unmounted card is NOCARD");
        require(call_owner(cpu, kCardReadAddress, {0u, 0u, 0x200u, 0x81008000u, 0u}) == kCardResultNoCard,
                "__CARDRead of an unmounted card is NOCARD");
        require(call_owner(cpu, kCardMountAsyncAddress, {1u, work, 0u, 0u}) == kCardResultNoCard &&
                    cpu.mem_read32(kCard0 + 0x110u + 4u) == kCardResultNoCard,
                "a mount of empty slot B fails NOCARD (EXIAttach)");

        cpu.mem_write8(0x800030E3u, 0x80u);
        require(call_owner(cpu, kCardProbeExAddress, {0u, 0u, 0u}) == kCardResultNoCard &&
                    call_owner(cpu, kCardMountAsyncAddress, {0u, work, 0u, 0u}) == kCardResultNoCard,
                "the boot's no-card-access flag makes the card absent");
        cpu.mem_write8(0x800030E3u, 0u);

        // An SRAM whose flash-ID checksum for slot A does not match: DoMount
        // takes DoUnmount's path with IOERROR, posting nothing.
        cpu.mem_write8(kScbAddress + 0x3Au, static_cast<u8>(cpu.mem_read8(kScbAddress + 0x3Au) ^ 1u));
        require(call_owner(cpu, kCardMountAsyncAddress, {0u, work, 0u, 0u}) == kCardResultIoError &&
                    cpu.mem_read32(kCard0) == 0u && cpu.mem_read32(kCard0 + 4u) == kCardResultIoError &&
                    cpu.mem_read32(kCard0 + 0x24u) == 0u &&
                    !system.interrupts().status(NativeInterrupts::Source::Card),
                "a flash-ID checksum mismatch fails the mount IOERROR");
        require(!cpu.stopped(), "nothing above stops the machine");
    }
    {
        // A second operation while the first one's completion is pending is
        // refused: the library's BUSY result serialises them.
        System system;
        system.start();
        system.set_memory_card_path(card_path);
        Cpu& cpu = system.cpu();
        cpu.gpr[13] = kTestSda;
        seed_mounted_card(cpu);
        call_owner(cpu, kCardReadAddress, {0u, 0u, 0x200u, 0x81008000u, 0u});
        call_owner(cpu, kCardReadAddress, {0u, 0x200u, 0x200u, 0x81008000u, 0u});
        require(cpu.stopped() && cpu.stop().reason == StopReason::ExecutionFault,
                "two card operations in flight on one channel stop the machine");
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// __OSInitSram and UnlockSram over the settings image; WriteUARTN's
// guest-visible effects; EXIInit's OS mask; the refused boot ROM and debugger.
void test_native_ipl_sram_and_uart() {
    const std::filesystem::path directory = fresh_test_directory("gekko-ipl-");
    const std::filesystem::path card_path = directory / "card.raw";
    write_test_file(card_path, make_test_card(64u));
    {
        System system;
        system.start();
        system.set_memory_card_path(card_path);
        Cpu& cpu = system.cpu();
        cpu.gpr[13] = kTestSda;
        const NativeIpl& ipl = system.ipl();
        require(ipl.settings_path() == directory / "SRAM.pc.raw", "commits persist beside the card's image");

        cpu.set_msr(cpu.msr() & ~kMsrEE);
        call_owner(cpu, kInitSramAddress, {});
        bool copied = true;
        for (u32 i = 0; i < 0x40u; ++i) copied = copied && cpu.mem_read8(kScbAddress + i) == ipl.settings()[4u + i];
        require(copied && cpu.mem_read32(kScbAddress + 0x40u) == 0x40u && cpu.mem_read32(kScbAddress + 0x48u) == 0u &&
                    cpu.mem_read32(kScbAddress + 0x4Cu) == 1u && cpu.mem_read32(kScbAddress + 0x44u) == 0u,
                "__OSInitSram: the 64 SRAM bytes in Scb, offset 64, unlocked, in sync, the lock's level");
        require(cpu.mem_read8(kScbAddress + 0x13u) == 0x04u && cpu.mem_read8(kScbAddress + 0x12u) == 0u,
                "the factory settings: English, stereo");

        // A lock taken with interrupts enabled, released without a commit.
        cpu.mem_write32(kScbAddress + 0x48u, 1u);
        cpu.mem_write32(kScbAddress + 0x44u, 1u);
        require(call_owner(cpu, kUnlockSramAddress, {0u, 0u}) == 1u && cpu.mem_read32(kScbAddress + 0x48u) == 0u &&
                    (cpu.msr() & kMsrEE) != 0u && ipl.stats().sram_persists == 0u,
                "UnlockSram(FALSE) releases the lock, restores the saved level and writes nothing");

        // A commit of the OSSram part: the flags clamp, the checksums, the write.
        cpu.set_msr(cpu.msr() & ~kMsrEE);
        cpu.mem_write32(kScbAddress + 0x48u, 1u);
        cpu.mem_write32(kScbAddress + 0x44u, 0u);
        cpu.mem_write8(kScbAddress + 0x12u, 2u);     // language
        cpu.mem_write8(kScbAddress + 0x13u, 0x07u);  // flags: video mode 3 is clamped to 0
        require(call_owner(cpu, kUnlockSramAddress, {1u, 0u}) == 1u, "a commit is in sync");
        require(cpu.mem_read8(kScbAddress + 0x13u) == 0x04u, "the commit clamps an invalid video mode");
        u16 sum = 0u, inverse = 0u;
        for (u32 p = 0x0Cu; p < 0x14u; p += 2u) {
            const u16 word = cpu.mem_read16(kScbAddress + p);
            sum = static_cast<u16>(sum + word);
            inverse = static_cast<u16>(inverse + static_cast<u16>(~word));
        }
        require(cpu.mem_read16(kScbAddress) == sum && cpu.mem_read16(kScbAddress + 2u) == inverse,
                "the commit writes the OSSram checksums over counterBias .. flags");
        bool written = true;
        for (u32 i = 0; i < 0x40u; ++i) written = written && ipl.settings()[4u + i] == cpu.mem_read8(kScbAddress + i);
        require(written && ipl.stats().sram_persists == 1u && cpu.mem_read32(kScbAddress + 0x40u) == 0x40u &&
                    cpu.mem_read32(kScbAddress + 0x48u) == 0u && (cpu.msr() & kMsrEE) == 0u,
                "the commit writes Scb.sram into the settings, persists once, and restores the level");
        const std::vector<u8> persisted = read_test_file(ipl.settings_path());
        require(persisted.size() == 0x44u && std::equal(persisted.begin(), persisted.end(), ipl.settings().begin()),
                "the persisted image is the settings image");
        require(call_owner(cpu, kUnlockSramAddress, {1u, 0u}) == 1u && ipl.stats().sram_persists == 1u &&
                    ipl.stats().sram_unchanged == 1u,
                "an identical commit does not persist again");

        // A commit from the OSSramEx part with an invalid gbs word clears it.
        cpu.mem_write16(kScbAddress + 0x3Cu, 0x5000u);
        require(call_owner(cpu, kUnlockSramAddress, {1u, 0x14u}) == 1u && cpu.mem_read16(kScbAddress + 0x3Cu) == 0u,
                "the commit's gbs rule clears an invalid value");

        // WriteUARTN: disabled -> 2 and nothing touched; enabled -> the
        // caller's '\n' become '\r', 0. UART text never reaches the SRAM.
        const char text[] = "ab\ncd\n";
        for (u32 i = 0; i < 6u; ++i) cpu.mem_write8(0x81005000u + i, static_cast<u8>(text[i]));
        cpu.mem_write32(kTestSda + 0x5EE0u, 0u);
        require(call_owner(cpu, kWriteUartNAddress, {0x81005000u, 6u}) == 2u && cpu.mem_read8(0x81005002u) == '\n',
                "WriteUARTN with the UART disabled returns 2 and leaves the buffer");
        cpu.mem_write32(kTestSda + 0x5EE0u, 0xA5FF005Au);
        const std::array<u8, NativeIpl::kSettingsSize> before = ipl.settings();
        require(call_owner(cpu, kWriteUartNAddress, {0x81005000u, 6u}) == 0u && cpu.mem_read8(0x81005002u) == '\r' &&
                    cpu.mem_read8(0x81005005u) == '\r' && cpu.mem_read8(0x81005000u) == 'a',
                "WriteUARTN rewrites every newline of the caller's buffer to a carriage return");
        require(ipl.stats().uart_lines == 1u && ipl.settings() == before, "the line is counted and the SRAM untouched");

        // EXIInit: the OS masks interrupts 9-16, nothing else.
        cpu.mem_write32(0x800000C4u, 0x00000010u);
        cpu.mem_write32(0x800000C8u, 0u);
        call_owner(cpu, kExiInitAddress, {});
        require(cpu.mem_read32(0x800000C4u) == (0x00000010u | 0x007F8000u) &&
                    system.interrupts().unmasked(NativeInterrupts::Source::Card),
                "EXIInit masks OS interrupts 9-16; the card's completion is not OS-maskable");
        require(!cpu.stopped(), "nothing above stops the machine");
    }
    for (const u32 refused : {kReadRomAddress, kInitMetroTrkAddress, kInitMetroTrkBbaAddress}) {
        System system;
        system.start();
        call_owner(system.cpu(), refused, {0x81001000u, 0x100u, 0u});
        require(system.cpu().stopped() && system.cpu().stop().reason == StopReason::ExecutionFault,
                "the boot ROM and the debugger hand-off are refused");
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// The card's completion ranks as the OS ranked EXI channel 0 (interrupt 9, the
// row holding ARAM, DSP, the EXI range, SI and DI) and is not OS-maskable.
// (Stage 6: no PI cause exists to compare with; the rank orders the native
// sources.)
void test_native_card_completion_rank() {
    System system;
    system.start();
    const u32 rows[] = {0x00000100u, 0x00000040u, 0xF8000000u, 0x00000200u, 0x00000080u, 0x00003000u,
                        0x00000020u, 0x03FF8C00u, 0x04000000u, 0x00004000u, 0xFFFFFFFFu};
    for (u32 i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) system.guest_memory().write32(0x805625C8u + 4u * i, rows[i]);
    Cpu& cpu = system.cpu();
    NativeInterrupts& interrupts = system.interrupts();
    using Source = NativeInterrupts::Source;
    cpu.lr = 0x81004000u;
    cpu.gpr[3] = 0x07000000u;  // the DSP group, every interrupt enabled
    cpu.gpr[4] = 0u;
    require(cpu.try_hle(0x8037620Cu) && cpu.gpr[3] == 0u, "the DSP group is applied");
    cpu.gpr[3] = 0x00007FE0u;  // the PI group, every interrupt enabled
    cpu.gpr[4] = 0u;
    require(cpu.try_hle(0x8037620Cu) && cpu.gpr[3] == 0u, "the PI group is applied");
    cpu.gpr[3] = 0x007F8000u;  // the EXI groups, every interrupt masked
    cpu.gpr[4] = 0xFFFFFFFFu;
    for (u32 guard = 0; guard < 4u && cpu.gpr[3] != 0u; ++guard) {
        require(cpu.try_hle(0x8037620Cu), "SetInterruptMask serves the EXI groups");
    }
    require(cpu.gpr[3] == 0u && interrupts.unmasked(Source::Card), "no OS mask group owns the card's completion");
    interrupts.raise(Source::Card);
    interrupts.raise(Source::AiDma);
    require(interrupts.next_source() == Source::Card, "the card's completion (9) precedes DSP_AI's row");
    interrupts.raise(Source::Dsp);
    require(interrupts.next_source() == Source::Dsp, "DSP (7) precedes the card (9) in their shared row");
    interrupts.clear(Source::Dsp);
    interrupts.raise(Source::PeFinish);
    require(interrupts.next_source() == Source::PeFinish, "PE's row precedes the card's");
}

// Stage 6 (docs/native-time-20261005/STAGE6_PI_MI.md, D2): __OSInterruptInit
// keeps the OS's interrupt bookkeeping - the handler-table pointer and its
// cleared table, the cleared masks, then every OS interrupt masked through the
// native mask path - and installs no external-interrupt handler: the
// exception table's entry 4 is not touched (this platform raises no external
// interrupt; the OS's dispatcher is pruned). No device is programmed.
void test_native_interrupt_init() {
    System system;
    system.start();
    Cpu& cpu = system.cpu();
    NativeInterrupts& interrupts = system.interrupts();
    using Source = NativeInterrupts::Source;
    cpu.gpr[13] = kTestSda;
    // Every native source unmasked first (the DSP and PI groups enabled).
    cpu.lr = 0x81004000u;
    cpu.gpr[3] = 0x07000000u;
    cpu.gpr[4] = 0u;
    require(cpu.try_hle(0x8037620Cu) && cpu.gpr[3] == 0u, "the DSP group is applied");
    cpu.gpr[3] = 0x00007FE0u;
    cpu.gpr[4] = 0u;
    require(cpu.try_hle(0x8037620Cu) && cpu.gpr[3] == 0u, "the PI group is applied");
    for (u32 offset = 0; offset < 0x80u; offset += 4u) cpu.mem_write32(0x80003040u + offset, 0x5A5A5A5Au);
    cpu.mem_write32(0x80003010u, 0x12345678u);  // the exception table's external-interrupt entry
    cpu.mem_write32(0x800000C4u, 0x0000FFFFu);
    cpu.mem_write32(0x800000C8u, 0x00FF0000u);

    call_owner(cpu, 0x80376198u, {});
    require(!cpu.stopped() && cpu.mem_read32(kTestSda + 0x5A90u) == 0x80003040u,
            "the OS's handler-table pointer is set");
    bool cleared = true;
    for (u32 offset = 0; offset < 0x80u; offset += 4u) cleared = cleared && cpu.mem_read32(0x80003040u + offset) == 0u;
    require(cleared, "the handler table is cleared");
    require(cpu.mem_read32(0x800000C4u) == 0xFFFFFFE0u && cpu.mem_read32(0x800000C8u) == 0u,
            "every OS interrupt masked, the user mask cleared");
    require(cpu.mem_read32(0x80003010u) == 0x12345678u, "no external-interrupt handler is installed");
    require(!interrupts.unmasked(Source::Vi) && !interrupts.unmasked(Source::DspDma) &&
                !interrupts.unmasked(Source::Dsp) && !interrupts.unmasked(Source::AiDma) &&
                !interrupts.unmasked(Source::PeFinish) && !interrupts.unmasked(Source::GxBreakpoint) &&
                interrupts.unmasked(Source::Card),
            "the native sources are masked through the OS's groups; the card's completion is never OS-masked");
}

// Stage 6 (STAGE6_PI_MI.md, D5): the reset controls. A PC process is a cold
// boot (OSGetResetCode 0, not a restart); the PC has no reset button
// (OSGetResetButtonState FALSE), and the kiosk auto-reset the boot information
// could ask for is refused; OSResetSystem ends the program naming the
// request; the memory interface's OnReset (an OS reset-queue record only
// OSResetSystem walks) is refused.
void test_native_reset_controls() {
    {
        System system;
        system.start();
        Cpu& cpu = system.cpu();
        cpu.mem_write8(0x800030E3u, 0u);
        require(call_owner(cpu, 0x803771B8u, {}) == 0u && !cpu.stopped(), "OSGetResetCode: a cold boot, code 0");
        cpu.gpr[3] = 0xDEADu;
        require(call_owner(cpu, 0x803772E4u, {}) == 0u && !cpu.stopped(), "OSGetResetButtonState: never pressed");
        require(call_owner(cpu, 0x803772E4u, {}) == 0u && !cpu.stopped(), "however often it is polled");
        require(system.reset_controls().stats().reset_button_polls == 2u &&
                    system.reset_controls().stats().reset_code_reads == 1u,
                "both are counted");
    }
    {
        System system;
        system.start();
        system.cpu().mem_write8(0x800030E3u, 0x05u);
        call_owner(system.cpu(), 0x803772E4u, {});
        require(system.cpu().stopped() && system.cpu().stop().reason == StopReason::ExecutionFault &&
                    system.cpu().stop().detail.find("kiosk") != std::string::npos,
                "the kiosk auto-reset is refused: " + system.cpu().stop().detail);
    }
    {
        System system;
        system.start();
        call_owner(system.cpu(), 0x80376FB8u, {1u, 0x40u, 0u});
        require(system.cpu().stopped() && system.cpu().stop().reason == StopReason::HleExit &&
                    system.cpu().stop().detail.find("hot reset") != std::string::npos &&
                    system.cpu().stop().detail.find("0x00000040") != std::string::npos,
                "OSResetSystem ends the program, naming the request: " + system.cpu().stop().detail);
    }
    {
        System system;
        system.start();
        call_owner(system.cpu(), 0x803769A0u, {1u});
        require(system.cpu().stopped() && system.cpu().stop().reason == StopReason::ExecutionFault,
                "the memory interface's OnReset is refused");
    }
}

// DISC_SYSTEM_MAP.md cut 1: what stays reachable of the game's disc-error screen module with no drive.
void test_native_disc_error_module() {
    {
        System system;
        system.start();
        Cpu& cpu = system.cpu();
        cpu.gpr[3] = 0xDEADu;
        require(call_owner(cpu, 0x8032D138u, {}) == 0u && !cpu.stopped(), "the screen never holds a load");
        for (const u32 state : {0u, 1u, 7u, 8u, 12u}) {
            require(call_owner(cpu, 0x8032D3D4u, {state}) == 1u && !cpu.stopped(),
                    "RtGCNFS's no-error states return 1 (state " + std::to_string(state) + ")");
        }
    }
    for (const u32 state : {2u, 6u}) {
        System system;
        system.start();
        call_owner(system.cpu(), 0x8032D3D4u, {state});
        require(system.cpu().stopped() && system.cpu().stop().reason == StopReason::ExecutionFault &&
                    system.cpu().stop().detail.find("drive state " + std::to_string(state)) != std::string::npos,
                "a drive error the native drive cannot produce is refused: " + system.cpu().stop().detail);
    }
}

// Stage 6 (STAGE6_PI_MI.md, D3): the OS handlers of interrupt sources this
// platform does not have stay in the OS's handler table, which nothing
// dispatches through; entered anyway, each refuses.
void test_native_refusing_interrupt_handlers() {
    const u32 handlers[] = {0x803769DCu,   // MEMIntrruptHandler
                            0x803771F0u,   // __OSResetSWInterruptHandler
                            0x803737F8u};  // __OSUnhandledException's protection-error report
    for (const u32 handler : handlers) {
        System system;
        system.start();
        call_owner(system.cpu(), handler, {4u, 0x80100000u});
        require(system.cpu().stopped() && system.cpu().stop().reason == StopReason::ExecutionFault &&
                    system.cpu().stop().detail.find("native interrupt delivery") != std::string::npos,
                "a handler of a source that does not exist refuses: " + system.cpu().stop().detail);
    }
}

// Stage 6 (STAGE6_PI_MI.md, D1): no device has registers. A guest access the
// Bus cannot serve stops the production machine, naming the retired device and
// the guest location, and never reaches the Bus; memory and registered
// fixtures are served as before; a unit fixture (no policy) keeps the counted
// unmapped access.
void test_unserved_guest_access_stops() {
    struct Page {
        u32 effective;
        const char* device;
    };
    const Page pages[] = {{0xCC000000u, "the command processor"}, {0xCC001000u, "the pixel engine"},
                          {0xCC002000u, "the video interface"},   {0xCC003000u, "the processor interface"},
                          {0xCC004000u, "the memory interface"},  {0xCC005000u, "the DSP interface"},
                          {0xCC006000u, "the disc interface"},    {0xCC006400u, "the serial interface"},
                          {0xCC006800u, "the external interface"}, {0xCC006C00u, "the audio interface"},
                          {0xCC00F000u, "an unassigned page"}};
    for (const Page& page : pages) {
        System system;
        system.start();
        (void)system.cpu().mem_read32(page.effective);
        const std::string& detail = system.cpu().stop().detail;
        require(system.cpu().stopped() && system.cpu().stop().reason == StopReason::ExecutionFault &&
                    detail.find(page.device) != std::string::npos && detail.find("guest pc=") != std::string::npos,
                std::string("a load from ") + page.device + " stops, naming it and the guest location: " + detail);
        require(system.cpu().unmapped_accesses_stopped() == 1u && system.guest_memory().diagnostics().unmapped_reads == 0u,
                "the access is counted and never reaches the Bus");
    }
    {
        System system;
        system.start();
        system.cpu().mem_write16(0xCC004010u, 0x00FFu);
        require(system.cpu().stopped() && system.cpu().stop().detail.find("store of 0xFF") != std::string::npos &&
                    system.cpu().stop().detail.find("the memory interface") != std::string::npos,
                "a store names its value and the device: " + system.cpu().stop().detail);
    }
    {
        System system;
        system.start();
        system.cpu().mem_write32(0x80100000u, 0x01020304u);
        require(system.cpu().mem_read32(0x80100000u) == 0x01020304u && !system.cpu().stopped(),
                "game memory is memory at its one address");
        (void)system.cpu().mem_read32(0xC0100000u);
        require(system.cpu().stopped() &&
                    system.cpu().stop().detail.find("a GameCube uncached-mirror address") != std::string::npos,
                "the GameCube's uncached mirror of that address maps nothing and stops: " +
                    system.cpu().stop().detail);
    }
    {
        struct Fixture16 : MmioDevice {
            const char* name() const override { return "fixture"; }
            u32 read32(u32) override { ++reads; return 0x0000ABCDu; }
            u32 reads = 0;
        } fixture;
        System system;
        system.start();
        system.guest_memory().register_mmio(0xCC003000u, 0x100u, &fixture);
        require(system.cpu().mem_read32(0xCC003000u) == 0x0000ABCDu && fixture.reads == 1u && !system.cpu().stopped(),
                "a registered fixture answers");
    }
    {
        Fixture fixture;
        fixture.bus.set_trace_unmapped(false);
        (void)fixture.cpu.mem_read32(0xCC003000u);
        require(!fixture.cpu.stopped() && fixture.bus.diagnostics().unmapped_reads == 1u,
                "without the policy (unit fixtures) the access is the Bus's counted unmapped access");
    }
}

void require_near(float actual, float expected, float slack, const std::string& message) {
    if (std::fabs(actual - expected) > slack) {
        throw std::runtime_error(message + " (got " + std::to_string(actual) + ", expected " +
                                 std::to_string(expected) + ")");
    }
}

void test_display_aspect_and_horplus() {
    require_near(aspect_ratio_for_mode(AspectMode::FourThree, 0.0f, 1920, 1080), 4.0f / 3.0f,
                 1.0e-5f, "4:3 mode must ignore the window");
    require_near(aspect_ratio_for_mode(AspectMode::SixteenNine, 0.0f, 640, 480), 16.0f / 9.0f,
                 1.0e-5f, "16:9 mode must ignore the window");
    require_near(aspect_ratio_for_mode(AspectMode::SixteenTen, 0.0f, 640, 480), 1.6f, 1.0e-5f,
                 "16:10 mode");
    require_near(aspect_ratio_for_mode(AspectMode::TwentyOneNine, 0.0f, 640, 480), 21.0f / 9.0f,
                 1.0e-5f, "21:9 mode");
    require_near(aspect_ratio_for_mode(AspectMode::ThirtyTwoNine, 0.0f, 640, 480), 32.0f / 9.0f,
                 1.0e-5f, "32:9 mode");
    require_near(aspect_ratio_for_mode(AspectMode::Auto, 0.0f, 1920, 1080), 16.0f / 9.0f, 1.0e-5f,
                 "Auto uses the output size");
    require_near(horplus_m00_scale(16.0f / 9.0f), 0.75f, 1.0e-5f,
                 "16:9 Hor+ must widen FOV by 4/3 over 16/9");
    require_near(horplus_m00_scale(4.0f / 3.0f), 1.0f, 1.0e-5f, "4:3 Hor+ is identity");
    require_near(horplus_m00_scale(1.0f), 1.0f, 1.0e-5f,
                 "a narrow target must not narrow the horizontal FOV");
}

void test_savestate_invalidates_unrestored_motion_blur_history() {
    System system;
    constexpr u32 kR13 = 0x80500000u;
    constexpr u32 kScreenEffect = 0x80510000u;
    constexpr u32 kBlurBackend = 0x80520000u;
    constexpr u32 kSingletonSlot = kR13 + 0x2DD8u;
    constexpr u32 kBackendSlot = kScreenEffect + 0x40u;
    constexpr u32 kHistoryValid = kBlurBackend + 0x78u;

    system.cpu().gpr[13] = kR13;
    GuestMemory& memory = system.guest_memory();
    memory.write32(kSingletonSlot, kScreenEffect);
    memory.write32(kBackendSlot, kBlurBackend);
    memory.write8(kHistoryValid, 1u);
    memory.write8(kHistoryValid + 1u, 0xA5u);

    require(invalidate_restored_shadow_render_history(system),
            "the restored Shadow screen-blur object must be discovered");
    require(memory.peek8(kHistoryValid) == 0u,
            "savestate load must invalidate temporal blur history absent from the GPU");
    require(memory.peek8(kHistoryValid + 1u) == 0xA5u,
            "history invalidation must change only the recovered validity byte");

    memory.write32(kSingletonSlot, 0u);
    require(!invalidate_restored_shadow_render_history(system),
            "an unconstructed effect singleton must be a safe no-op");
}

void test_display_hud_anchors_and_scale() {
    require(classify_hud_anchor({20.0f, 16.0f, 90.0f, 70.0f}, true, false) == HudAnchor::TopLeft,
            "lives stay top-left");
    require(classify_hud_anchor({250.0f, 8.0f, 390.0f, 48.0f}, true, false) == HudAnchor::Top,
            "time stays top-center");
    require(classify_hud_anchor({12.0f, 390.0f, 170.0f, 468.0f}, true, false) ==
                HudAnchor::BottomLeft,
            "hero meter stays bottom-left");
    require(classify_hud_anchor({470.0f, 390.0f, 628.0f, 468.0f}, true, false) ==
                HudAnchor::BottomRight,
            "dark meter stays bottom-right");
    require(classify_hud_anchor({0.0f, 0.0f, 640.0f, 480.0f}, true, false) == HudAnchor::Canvas,
            "textured full-screen 2D stays a 4:3 canvas on menu frames");
    require(classify_hud_anchor({0.0f, 0.0f, 640.0f, 480.0f}, false, true) == HudAnchor::Fill,
            "untextured full-screen 2D covers the output (fades)");
    // Menus keep their original composition (user 2026-10-28; the 2026-10-26 edge-art rule is gone): tall art flush
    // with a canvas side stays in the centred 4:3 canvas with the rest of the menu. Bounds: the results' vertical
    // RESULT lettering (Lethal Highway, frame 16520) and a panel flush right, as the Options menu's.
    {
        shadow::ui::HudLayout layout;
        layout.on_group(shadow::ui::HudGroupKind::CsdWidget, 1u, 0u, 320.0f);
        require(layout.layout_draw({-0.5f, 14.0f, 150.5f, 463.4f}, true).anchor == HudAnchor::Center &&
                    layout.layout_draw({500.0f, 20.0f, 640.5f, 460.0f}, true).anchor == HudAnchor::Center,
                "tall side art stays in the menu's 4:3 canvas");
        layout.end_frame();
    }
    // The canvas the rules measure is the bracket's raster, not a fixed 640 x 480 (2026-10-27, docs/ASPECT_INVENTORY.md
    // U1): a split-screen player's 640 x 240 view and the PAL 50 Hz 640 x 528 screen.
    require(shadow::ui::spans_canvas_height({0.0f, 0.0f, 64.0f, 240.0f}, 240.0f) &&
                !shadow::ui::spans_canvas_height({0.0f, 0.0f, 64.0f, 240.0f}),
            "a column over a 240-row view's height spans that view, not a 480-row canvas");
    require(classify_hud_anchor({0.0f, 0.0f, 640.0f, 528.0f}, true, false, 640.0f, 528.0f) == HudAnchor::Canvas &&
                classify_hud_anchor({20.0f, 450.0f, 90.0f, 510.0f}, true, false, 640.0f, 528.0f) ==
                    HudAnchor::BottomLeft,
            "the 528-row canvas is classified in its own rows");
    {
        shadow::ui::HudLayout layout;
        layout.set_regions(nullptr, 0u, 640.0f, 240.0f);
        require(layout.layout_draw({0.0f, 0.0f, 640.0f, 240.0f}, true).anchor == HudAnchor::Canvas,
                "a background over a half-height view is that view's canvas");
        layout.set_regions(nullptr, 0u, 320.0f, 240.0f);
        require(layout.layout_draw({300.0f, 0.0f, 400.0f, 10.0f}, true).laid_out &&
                    !layout.layout_draw({330.0f, 0.0f, 400.0f, 10.0f}, true).laid_out,
                "off-canvas is past the bracket's own width");
    }
    // Backgrounds (2026-10-28): a textured draw over the whole canvas stays the centred 4:3 canvas, as the original
    // shows it; an untextured one (a fade) fills.
    {
        shadow::ui::HudLayout layout;
        require(layout.layout_draw({0.0f, 0.0f, 640.0f, 480.0f}, true).anchor == HudAnchor::Canvas,
                "a full-screen background stays the 4:3 canvas, neither covered nor stretched");
        require(layout.layout_draw({0.0f, 0.0f, 640.0f, 480.0f}, false).anchor == HudAnchor::Fill,
                "a full-screen fade still fills");
        require(layout.layout_draw({0.0f, 200.0f, 640.0f, 260.0f}, true).anchor == HudAnchor::Fill,
                "a full-width textured band still fills its width");
        layout.end_frame();
    }
    // A sprite zoomed past both canvas sides (the title screen's after-images of the SHADOW wordmark, honban.gncp
    // "Title" title2..title6: 512 x 128 about (287, 191), scaled up) is content larger than the screen: it keeps its
    // widget's anchor, as the wordmark it echoes does, instead of being stretched to the view's width.
    {
        require(shadow::ui::overflows_canvas_width({-125.0f, 95.0f, 699.0f, 287.0f}) &&
                    !shadow::ui::overflows_canvas_width({0.0f, 200.0f, 640.0f, 260.0f}) &&
                    !shadow::ui::overflows_canvas_width({-6.0f, 200.0f, 646.0f, 260.0f}) &&
                    shadow::ui::overflows_canvas_width({-72.4f, 101.4f, 646.4f, 280.6f}),
                "overflow means past a side, beyond the frame's slack");
        shadow::ui::HudLayout layout;
        layout.on_group(shadow::ui::HudGroupKind::CsdWidget, 7u, 0u, 320.0f);
        require(layout.layout_draw({31.0f, 127.0f, 543.0f, 255.0f}, true).anchor == HudAnchor::Center,
                "the wordmark is centred with its widget");
        require(layout.layout_draw({-125.0f, 95.0f, 699.0f, 287.0f}, true).anchor == HudAnchor::Center,
                "its zoomed after-image stays with it");
        require(layout.layout_draw({-72.4f, 101.4f, 646.4f, 280.6f}, true).beyond_canvas &&
                    !layout.layout_draw({31.0f, 127.0f, 543.0f, 255.0f}, true).beyond_canvas,
                "only the zoomed copy is cut at the view's edge rather than the canvas box's");
        require(layout.layout_draw({-72.4f, 101.4f, 646.4f, 280.6f}, true).anchor == HudAnchor::Center,
                "so does one only a few units past the right edge (title entrance, frame 2178)");
        require(layout.layout_draw({-200.0f, 200.0f, 840.0f, 260.0f}, false).anchor == HudAnchor::Fill,
                "an untextured band past the edges still fills");
        require(layout.layout_draw({-200.0f, -100.0f, 840.0f, 580.0f}, true).anchor == HudAnchor::Canvas,
                "a textured draw over the whole canvas is still the canvas");
        require(layout.stats().zoomed_sprites == 3u, "three zoomed-sprite draws counted");
        layout.end_frame();
        shadow::ui::HudLayout old_rule;
        old_rule.set_zoomed_sprites_fill(true);
        require(old_rule.layout_draw({-125.0f, 95.0f, 699.0f, 287.0f}, true).anchor == HudAnchor::Fill,
                "the A/B switch restores the earlier fill");
    }
    {
        DisplayLayout wide;
        wide.output_width = 1920;
        wide.output_height = 1080;
        wide.aspect = 16.0f / 9.0f;
        const HudAffine fill = hud_affine_for_anchor(HudAnchor::Fill, wide);
        require_near(apply_hud_x(fill, 20.0f), 20.0f, 0.01f,
                     "gameplay full-screen overlays must not compress into gutters");
        const HudAffine left = hud_affine_for_anchor(HudAnchor::BottomLeft, wide);
        require(apply_hud_x(left, 20.0f) < 20.0f,
                "left HUD must move toward the output left edge in the EFB");
    }

    DisplayLayout native;
    native.output_width = 640;
    native.output_height = 480;
    native.aspect = kNativeAspect;
    const HudAffine identity = hud_affine_for_anchor(HudAnchor::Left, native);
    require_near(apply_hud_x(identity, 20.0f), 20.0f, 0.01f, "640x480 left HUD is identity");
    require_near(apply_hud_x(identity, 320.0f), 320.0f, 0.01f, "640x480 center HUD is identity");

    const struct {
        int width;
        int height;
        float aspect;
    } outputs[] = {
        {640, 480, 4.0f / 3.0f},     {1280, 720, 16.0f / 9.0f},  {1920, 1080, 16.0f / 9.0f},
        {2560, 1440, 16.0f / 9.0f}, {2560, 1080, 21.0f / 9.0f}, {3840, 1080, 32.0f / 9.0f},
    };
    for (const auto& output : outputs) {
        DisplayLayout layout;
        layout.output_width = output.width;
        layout.output_height = output.height;
        layout.aspect = output.aspect;
        layout.hud_scale = 1.0f;
        const float present_scale =
            static_cast<float>(output.width) / kVirtualHudWidth;
        const float pixel = static_cast<float>(output.height) / kVirtualHudHeight;
        const HudAffine left = hud_affine_for_anchor(HudAnchor::BottomLeft, layout);
        const HudAffine right = hud_affine_for_anchor(HudAnchor::BottomRight, layout);
        const HudAffine center = hud_affine_for_anchor(HudAnchor::Top, layout);
        const float left_out = apply_hud_x(left, 20.0f) * present_scale;
        const float lives_width =
            (apply_hud_x(left, 60.0f) - apply_hud_x(left, 20.0f)) * present_scale;
        const float center_out = apply_hud_x(center, 320.0f) * present_scale;
        const float right_out = apply_hud_x(right, 620.0f) * present_scale;
        require_near(left_out, 20.0f * pixel, 1.5f, "left HUD stays at the left edge");
        require_near(lives_width, 40.0f * pixel, 1.5f,
                     "HUD element width follows height, not aspect");
        require_near(center_out, static_cast<float>(output.width) * 0.5f, 1.5f,
                     "time stays centred");
        require_near(right_out,
                     static_cast<float>(output.width) - 20.0f * pixel, 1.5f,
                     "right HUD stays at the right edge");
    }
}

void test_display_widescreen_layout_invariants() {
    // The 2D layout is defined entirely in the 640x480 virtual canvas, as
    //     x' = anchor + s * (x - anchor),  s = (4:3) / target_aspect
    // and the same s scales the 3D projection's horizontal term. These are the
    // invariants that make the layout resolution-independent, keep the timer on
    // the exact centre, and keep world-projected 2D attached to its object.
    const struct {
        const char* name;
        float aspect;
    } targets[] = {
        {"4:3", 4.0f / 3.0f},   {"16:10", 16.0f / 10.0f}, {"16:9", 16.0f / 9.0f},
        {"21:9", 21.0f / 9.0f}, {"32:9", 32.0f / 9.0f},   {"2.37", 2.37f},
    };
    // Same aspect, wildly different resolutions: the canvas-space layout must
    // not depend on the resolution at all. This is what makes per-resolution
    // positional hacks unnecessary.
    const struct {
        int width;
        int height;
    } sizes[] = {{640, 480}, {1280, 720}, {1920, 1080}, {2560, 1440},
                 {3840, 2160}, {2560, 1080}, {3440, 1440}, {1100, 823}};

    for (const auto& target : targets) {
        const float scale = horplus_m00_scale(target.aspect);
        HudAffine reference{};
        bool have_reference = false;
        for (const auto& size : sizes) {
            DisplayLayout layout;
            layout.output_width = size.width;
            layout.output_height = size.height;
            layout.aspect = target.aspect;
            layout.hud_scale = 1.0f;
            layout.hud_inset = 0.0f;

            const HudAffine left = hud_affine_for_anchor(HudAnchor::Left, layout);
            const HudAffine centre = hud_affine_for_anchor(HudAnchor::Center, layout);
            const HudAffine right = hud_affine_for_anchor(HudAnchor::Right, layout);

            // x' = anchor + s * (x - anchor), for anchors 0, 320 and 640.
            for (float x = 0.0f; x <= kVirtualHudWidth; x += 64.0f) {
                require_near(apply_hud_x(left, x), scale * x, 1.0e-3f,
                             std::string("left anchor is a scale about x=0 at ") + target.name);
                require_near(apply_hud_x(centre, x),
                             kVirtualHudWidth * 0.5f + scale * (x - kVirtualHudWidth * 0.5f),
                             1.0e-3f,
                             std::string("centre anchor is a scale about x=320 at ") +
                                 target.name);
                require_near(apply_hud_x(right, x),
                             kVirtualHudWidth - scale * (kVirtualHudWidth - x), 1.0e-3f,
                             std::string("right anchor is a scale about x=640 at ") +
                                 target.name);
                // Vertical composition is never touched.
                require_near(apply_hud_y(centre, x * 0.75f), x * 0.75f, 1.0e-3f,
                             "the layout must not move anything vertically");
            }
            // The edges themselves are fixed points, so a widget keeps its
            // physical margin from the screen edge it belongs to.
            require_near(apply_hud_x(left, 0.0f), 0.0f, 1.0e-3f, "left edge is a fixed point");
            require_near(apply_hud_x(right, kVirtualHudWidth), kVirtualHudWidth, 1.0e-3f,
                         "right edge is a fixed point");
            require_near(apply_hud_x(centre, kVirtualHudWidth * 0.5f), kVirtualHudWidth * 0.5f,
                         1.0e-3f, "the canvas centre is a fixed point, so the timer stays centred");

            if (!have_reference) {
                reference = centre;
                have_reference = true;
            } else {
                require_near(centre.ax, reference.ax, 1.0e-6f,
                             "canvas layout must not depend on the output resolution");
                require_near(centre.bx, reference.bx, 1.0e-6f,
                             "canvas layout must not depend on the output resolution");
            }
        }
    }

    // The centre anchor must be exactly the transform the 3D projection gets:
    // scaling clip x by s maps canvas x to 320 + s * (x - 320). This is what
    // keeps a lock-on mark or a hint arrow on the object it points at.
    {
        DisplayLayout layout;
        layout.output_width = 1920;
        layout.output_height = 1080;
        layout.aspect = 16.0f / 9.0f;
        const HudAffine centre = hud_affine_for_anchor(HudAnchor::Center, layout);
        const float scale = horplus_m00_scale(render_content_aspect(layout));
        for (float ndc = -1.0f; ndc <= 1.0f; ndc += 0.25f) {
            const float canvas = kVirtualHudWidth * 0.5f * (ndc + 1.0f);
            const float projected = kVirtualHudWidth * 0.5f * (scale * ndc + 1.0f);
            require_near(apply_hud_x(centre, canvas), projected, 1.0e-3f,
                         "centre-anchored 2D must land where the widened 3D projection does");
        }
    }

    // A forced aspect that disagrees with the window must drive the 2D layout
    // too, because it drives the 3D projection. Deriving the affine from
    // output_width / output_height instead would counter-scale the HUD by a
    // different amount than the scene it sits on.
    {
        DisplayLayout forced;
        forced.output_width = 1000;
        forced.output_height = 1000;
        forced.aspect = 16.0f / 9.0f;
        const HudAffine centre = hud_affine_for_anchor(HudAnchor::Center, forced);
        require_near(centre.ax, horplus_m00_scale(16.0f / 9.0f), 1.0e-5f,
                     "the HUD follows layout.aspect, not the window shape");
    }

    // Hor+ never narrows: a target narrower than 4:3 keeps the original
    // frustum and is pillarboxed by the presenter instead of being cropped.
    require_near(render_content_aspect([] {
                     DisplayLayout tall;
                     tall.aspect = 1.0f;
                     return tall;
                 }()),
                 kNativeAspect, 1.0e-5f, "a narrow window must not crop the sides");
    require_near(render_content_aspect([] {
                     DisplayLayout wide;
                     wide.aspect = 21.0f / 9.0f;
                     return wide;
                 }()),
                 21.0f / 9.0f, 1.0e-5f, "a wide window widens");

    // Full-frame draws - fades, overlays, backgrounds - must keep covering the
    // whole viewport, so they are never anchored.
    {
        DisplayLayout layout;
        layout.output_width = 3440;
        layout.output_height = 1440;
        layout.aspect = 21.0f / 9.0f;
        const HudAffine fill = hud_affine_for_anchor(HudAnchor::Fill, layout);
        require_near(apply_hud_x(fill, 0.0f), 0.0f, 1.0e-4f, "a fade starts at the left edge");
        require_near(apply_hud_x(fill, kVirtualHudWidth), kVirtualHudWidth, 1.0e-4f,
                     "a fade reaches the right edge");
        require_near(apply_hud_y(fill, kVirtualHudHeight), kVirtualHudHeight, 1.0e-4f,
                     "a fade reaches the bottom edge");
    }

    // 640x480 4:3 has to be exactly the unmodified game, so every anchor is
    // the identity and horplus_m00_scale is exactly 1.0f - not merely close.
    {
        DisplayLayout native;
        native.output_width = 640;
        native.output_height = 480;
        native.aspect = kNativeAspect;
        require(horplus_m00_scale(kNativeAspect) == 1.0f,
                "the 4:3 Hor+ factor must be exactly 1.0f so the projection is bit-identical");
        for (const HudAnchor anchor :
             {HudAnchor::Left, HudAnchor::Center, HudAnchor::Right, HudAnchor::TopLeft,
              HudAnchor::BottomRight, HudAnchor::Fill}) {
            const HudAffine affine = hud_affine_for_anchor(anchor, native);
            require(affine.ax == 1.0f && affine.bx == 0.0f && affine.ay == 1.0f &&
                        affine.by == 0.0f,
                    "640x480 4:3 must be the identity for every anchor");
        }
    }
}

void test_display_group_coverage_and_anchors() {
    // Coverage is a per-axis property. The recovered fade submits its curtain
    // as ten 64x480 columns and a full-width band can be any height, so the
    // width test must not be gated on the height.
    require(spans_canvas_width({0.0f, 0.0f, kVirtualHudWidth, 40.0f}),
            "a full-width band 40 units tall still covers the screen horizontally");
    require(spans_canvas_width({0.0f, 0.0f, kVirtualHudWidth, kVirtualHudHeight}),
            "a full-canvas quad covers the screen horizontally");
    require(!spans_canvas_width({0.0f, 0.0f, 64.0f, kVirtualHudHeight}),
            "one 64-wide column of a curtain is not itself full width");
    require(spans_canvas_height({0.0f, 0.0f, 64.0f, kVirtualHudHeight}),
            "one column of the curtain does span the canvas height");
    require(!spans_canvas_height({456.0f, 274.0f, 565.0f, 409.0f}),
            "the loading graphic drawn on top of the curtain does not");

    // A widget lying wholly in one half of the canvas belongs to that side,
    // however close to the middle it reaches. The meter icon that caps the
    // Hero gauge sits at 197.5..229.5 - its centroid is within half a unit of
    // the left third, so a centroid rule detaches it from its gauge.
    require(classify_group_anchor({197.5f, 40.0f, 229.5f, 80.0f}, HudAnchor::Center, false) ==
                HudAnchor::Left,
            "a widget wholly in the left half is left-anchored");
    require(classify_group_anchor({415.5f, 40.0f, 447.5f, 80.0f}, HudAnchor::Center, false) ==
                HudAnchor::Right,
            "a widget wholly in the right half is right-anchored");
    require(classify_group_anchor({43.5f, 40.0f, 231.5f, 90.0f}, HudAnchor::Center, false) ==
                HudAnchor::Left,
            "the Hero gauge is left-anchored");
    require(classify_group_anchor({410.5f, 40.0f, 599.0f, 90.0f}, HudAnchor::Center, false) ==
                HudAnchor::Right,
            "the Dark gauge is right-anchored");
    require(classify_group_anchor({24.0f, 60.0f, 620.0f, 420.0f}, HudAnchor::Center, false) ==
                HudAnchor::Center,
            "a menu authored across the frame stays a centred safe area");

    // A run of HUD text reports one alignment point, not an extent, so it is
    // classified by thirds. These are the eight X values the gameplay HUD uses.
    const auto point = [](float x) { return HudBounds{x, 0.0f, x, 0.0f}; };
    for (const float x : {54.0f, 78.0f, 84.0f, 120.0f}) {
        require(classify_group_anchor(point(x), HudAnchor::Center, false) == HudAnchor::Left,
                "left-hand HUD text");
    }
    require(classify_group_anchor(point(320.0f), HudAnchor::Center, false) == HudAnchor::Center,
            "the timer sits on the canvas centre and must stay centred");
    for (const float x : {477.0f, 520.0f, 560.0f}) {
        require(classify_group_anchor(point(x), HudAnchor::Center, false) == HudAnchor::Right,
                "right-hand HUD text");
    }

    // A text run is classified from its alignment point alone, with no
    // hysteresis: every gameplay glyph site reads a constant X out of the
    // recovered layout table, so there is nothing to hold steady, and holding
    // one would make a settled run sticky between a third and the middle.
    require(classify_group_anchor(point(220.0f), HudAnchor::Left, true) == HudAnchor::Center,
            "a text run follows its own X, remembered anchor or not");
    require(classify_group_anchor(point(560.0f), HudAnchor::Left, true) == HudAnchor::Right,
            "and does so in both directions");

    // Widgets DO move, so a settled widget only changes side once it is
    // decisively there - its centre a quarter of the canvas past the middle.
    // A widget that has merely drifted into the left half keeps the anchor it
    // had, which is what stops an entrance animation popping mid-slide.
    require(classify_group_anchor({30.0f, 0.0f, 300.0f, 40.0f}, HudAnchor::Center, true) ==
                HudAnchor::Center,
            "drifting into the left half is not enough to take the left anchor");
    require(classify_group_anchor({30.0f, 0.0f, 250.0f, 40.0f}, HudAnchor::Center, true) ==
                HudAnchor::Left,
            "but a decisive move to the left does");
    require(classify_group_anchor({500.0f, 0.0f, 620.0f, 40.0f}, HudAnchor::Left, true) ==
                HudAnchor::Right,
            "and a decisive move to the right takes the right anchor");
    require(classify_group_anchor({150.0f, 0.0f, 400.0f, 40.0f}, HudAnchor::Left, true) ==
                HudAnchor::Center,
            "a widget that is no longer decisively to one side becomes centred");
    // A widget that used to hug an edge and is now a centred composition - the
    // stage-title card sharing a draw site with the gauge meter icons - must
    // not inherit the edge anchor.
    require(classify_group_anchor({120.0f, 200.0f, 520.0f, 280.0f}, HudAnchor::Left, true) ==
                HudAnchor::Center,
            "a centred card must not inherit a remembered edge anchor");
}

void test_display_fit_and_config_parse() {
    const ContentRect fill = fit_content_rect(1920, 1080, 16.0f / 9.0f);
    require(fill.x == 0 && fill.y == 0 && fill.width == 1920 && fill.height == 1080,
            "16:9 content fills a 16:9 window");
    const ContentRect pillar = fit_content_rect(1920, 1080, 4.0f / 3.0f);
    require(pillar.width == 1440 && pillar.height == 1080 && pillar.x == 240 && pillar.y == 0,
            "4:3 content pillarboxes in 16:9");
    const ContentRect letter = fit_content_rect(1920, 1080, 21.0f / 9.0f);
    require(letter.width == 1920 && letter.x == 0 && letter.y > 0,
            "21:9 content letterboxes in 16:9");

    DisplayConfig config;
    std::string error;
    require(parse_display_file("width=2560\nheight=1440\naspect=21:9\nhud_scale=1.25\n"
                               "hud_inset=32\nfullscreen=true\n",
                               config, &error),
            error.empty() ? "display.cfg must parse" : error);
    require(config.window_width == 2560 && config.window_height == 1440 && config.fullscreen &&
                config.aspect == AspectMode::TwentyOneNine &&
                std::fabs(config.hud_scale - 1.25f) < 1.0e-5f && config.hud_inset == 32.0f,
            "display.cfg keys must apply");
    bool ok = false;
    require(parse_aspect_mode("16:10", &ok) == AspectMode::SixteenTen && ok,
            "16:10 aspect token");
}

void test_display_internal_scale_and_fitted_hud_inset() {
    const struct {
        int width;
        int height;
        float aspect;
        u32 scale;
    } cases[] = {
        {640, 480, 4.0f / 3.0f, 1u},
        {1280, 720, 16.0f / 9.0f, 2u},
        {1920, 1080, 16.0f / 9.0f, 3u},
        {2560, 1440, 16.0f / 9.0f, 4u},
        {2560, 1080, 21.0f / 9.0f, 4u},
        {3840, 2160, 16.0f / 9.0f, 6u},
        {7680, 4320, 16.0f / 9.0f, 6u},
    };
    for (const auto& item : cases) {
        DisplayLayout layout;
        layout.output_width = item.width;
        layout.output_height = item.height;
        layout.aspect = item.aspect;
        require(recommended_internal_scale(layout) == item.scale,
                "auto internal scale must cover both fitted output axes");
    }

    // Shadow's recovered SaveScreen/DrawBlur contract is a logical 320x240
    // history inside the anamorphic 640x480 guest raster. At 1920x1080 auto
    // selects 3x, so the GPU-resident history is 960x720. Presentation maps
    // the 1440-high active guest raster to 1080 output lines, making that
    // history exactly 960x540 on screen. Allocating a 960x540 resident history
    // instead would apply the 16:9 correction twice and corrupt its UVs.
    DisplayLayout hd;
    hd.output_width = 1920;
    hd.output_height = 1080;
    hd.aspect = 16.0f / 9.0f;
    const u32 hd_scale = recommended_internal_scale(hd);
    const u32 resident_history_width = 320u * hd_scale;
    const u32 resident_history_height = 240u * hd_scale;
    const ContentRect hd_content =
        fit_content_rect(hd.output_width, hd.output_height, render_content_aspect(hd));
    const u32 presented_history_width =
        resident_history_width * static_cast<u32>(hd_content.width) / (640u * hd_scale);
    const u32 presented_history_height =
        resident_history_height * static_cast<u32>(hd_content.height) / (480u * hd_scale);
    require(resident_history_width == 960u && resident_history_height == 720u,
            "3x resident DrawBlur history must preserve the logical 4:3 raster");
    require(presented_history_width == 960u && presented_history_height == 540u,
            "1920x1080 presentation must show the DrawBlur history as exactly 960x540");

    // A forced 4:3 image in a 1920x1080 surface is 1440 pixels wide. An inset
    // is measured inside that content, not inside the 1920-wide swapchain.
    DisplayLayout pillar;
    pillar.output_width = 1920;
    pillar.output_height = 1080;
    pillar.aspect = 4.0f / 3.0f;
    pillar.hud_inset = 32.0f;
    const ContentRect content = fit_content_rect(
        pillar.output_width, pillar.output_height, render_content_aspect(pillar));
    const HudAffine left = hud_affine_for_anchor(HudAnchor::Left, pillar);
    const float content_pixels_per_canvas =
        static_cast<float>(content.width) / kVirtualHudWidth;
    require_near(apply_hud_x(left, 0.0f) * content_pixels_per_canvas, 32.0f, 0.01f,
                 "HUD inset must remain an exact content-pixel distance when pillarboxed");
}

// The presenter's upscale path is the frame-rate limiter at any output size
// other than 1:1, so it is written for speed: the horizontal taps are hoisted
// out of the inner loop and the rows run on the host job pool. Neither is
// allowed to change a single output byte. This pins that: the reference below
// is the original straight-line per-pixel form, evaluated in the same order,
// and the shipped function must reproduce it exactly.
void scale_rgba_bilinear_reference(const std::uint32_t* source, int source_width,
                                   int source_height, std::uint32_t* dest, int dest_stride,
                                   int dest_x, int dest_y, int dest_width, int dest_height) {
    const float x_scale = static_cast<float>(source_width) / static_cast<float>(dest_width);
    const float y_scale = static_cast<float>(source_height) / static_cast<float>(dest_height);
    for (int y = 0; y < dest_height; ++y) {
        const float src_y = (static_cast<float>(y) + 0.5f) * y_scale - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(src_y)), 0, source_height - 1);
        const int y1 = std::min(y0 + 1, source_height - 1);
        const float ty = std::clamp(src_y - static_cast<float>(y0), 0.0f, 1.0f);
        std::uint32_t* row = dest + (dest_y + y) * dest_stride + dest_x;
        for (int x = 0; x < dest_width; ++x) {
            const float src_x = (static_cast<float>(x) + 0.5f) * x_scale - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(src_x)), 0, source_width - 1);
            const int x1 = std::min(x0 + 1, source_width - 1);
            const float tx = std::clamp(src_x - static_cast<float>(x0), 0.0f, 1.0f);
            const auto sample = [&](int sx, int sy) { return source[sy * source_width + sx]; };
            const auto channel = [](std::uint32_t pixel, int shift) {
                return static_cast<float>((pixel >> shift) & 0xffu);
            };
            const auto blend = [&](int shift) {
                const float a = channel(sample(x0, y0), shift);
                const float b = channel(sample(x1, y0), shift);
                const float c = channel(sample(x0, y1), shift);
                const float d = channel(sample(x1, y1), shift);
                const float top = a + (b - a) * tx;
                const float bottom = c + (d - c) * tx;
                return static_cast<std::uint32_t>(
                    std::clamp(top + (bottom - top) * ty, 0.0f, 255.0f) + 0.5f);
            };
            row[x] = (blend(24) << 24) | (blend(16) << 16) | (blend(8) << 8) | blend(0);
        }
    }
}

void test_display_scale_bilinear_matches_reference() {
    const int source_width = 64;
    const int source_height = 48;
    std::vector<std::uint32_t> source(static_cast<std::size_t>(source_width) * source_height);
    std::uint32_t seed = 0x1234567u;
    for (std::uint32_t& pixel : source) {
        seed = seed * 1664525u + 1013904223u;
        pixel = seed;
    }

    struct Case {
        int width;
        int height;
        int x;
        int y;
        const char* what;
    };
    // The 1440x1080 case is the shipping pillarboxed 4:3-in-16:9 geometry and
    // the only one tall enough to take the parallel path (>= 64 rows).
    const Case cases[] = {
        {1440, 1080, 240, 0, "pillarboxed 1080p upscale"},
        {192, 144, 5, 7, "small offset upscale"},
        {32, 24, 0, 0, "downscale"},
        {64, 63, 1, 1, "just under the parallel threshold"},
        {64, 64, 0, 0, "exactly at the parallel threshold"},
    };

    for (const Case& c : cases) {
        const int stride = c.x + c.width + 3;  // deliberately not tight
        const std::size_t span = static_cast<std::size_t>(stride) * (c.y + c.height);
        std::vector<std::uint32_t> actual(span, 0xCDCDCDCDu);
        std::vector<std::uint32_t> expected(span, 0xCDCDCDCDu);
        scale_rgba_bilinear(source.data(), source_width, source_height, actual.data(), stride,
                            c.x, c.y, c.width, c.height);
        scale_rgba_bilinear_reference(source.data(), source_width, source_height,
                                      expected.data(), stride, c.x, c.y, c.width, c.height);
        require(actual == expected,
                std::string("scale_rgba_bilinear must stay bit-identical: ") + c.what);
    }

    // The equal-size path is a straight memcpy and must not touch padding.
    const int stride = source_width + 2;
    std::vector<std::uint32_t> copied(static_cast<std::size_t>(stride) * source_height,
                                      0xCDCDCDCDu);
    scale_rgba_bilinear(source.data(), source_width, source_height, copied.data(), stride, 0, 0,
                        source_width, source_height);
    bool copy_exact = true;
    for (int y = 0; y < source_height && copy_exact; ++y) {
        for (int x = 0; x < source_width; ++x) {
            if (copied[static_cast<std::size_t>(y) * stride + x] !=
                source[static_cast<std::size_t>(y) * source_width + x]) {
                copy_exact = false;
                break;
            }
        }
        if (copied[static_cast<std::size_t>(y) * stride + source_width] != 0xCDCDCDCDu) {
            copy_exact = false;
        }
    }
    require(copy_exact, "equal-size scale copies rows exactly and leaves padding alone");
}

void test_render_geometry_internal_scale_copy_contract() {
    for (const u32 factor : {1u, 2u, 3u, 4u, 6u}) {
        const EfbGeometry efb{RenderScale{factor}};
        require(efb.native_width() == kGuestEfbWidth * factor &&
                    efb.native_height() == kGuestEfbHeight * factor,
                "internal EFB dimensions must scale from guest space exactly once");
        require(efb.native_raster() ==
                    PixelRect{0u, 0u, kGuestRasterWidth * factor,
                              kGuestRasterHeight * factor},
                "the programmed guest raster must map to the matching native rectangle");

        const PixelRect request{16u, 8u, 320u, 240u};
        const ResolvedCopy copy = resolve_copy(efb, request, false, 6u);
        require(copy.source_guest == request &&
                    copy.source_native == PixelRect{16u * factor, 8u * factor,
                                                    320u * factor, 240u * factor} &&
                    copy.destination == PixelRect{0u, 0u, 320u, 240u} &&
                    copy.consistent() && !copy.clamped,
                "EFB copies must read native pixels but retain guest texture dimensions");
    }

    const EfbGeometry efb{RenderScale{3u}};
    const ResolvedCopy clamped =
        resolve_copy(efb, PixelRect{630u, 520u, 32u, 32u}, true, 6u);
    require(clamped.source_guest == PixelRect{630u, 520u, 10u, 8u} &&
                clamped.source_native == PixelRect{1890u, 1560u, 30u, 24u} &&
                clamped.destination == PixelRect{0u, 0u, 10u, 8u} && clamped.clamped &&
                clamped.consistent(),
            "one guest-space clamp must drive native source and guest destination together");
}


// The presentation blit is the only path that reaches the window, and until
// 2026-09-01 it had no gate at all: --screenshot writes the guest XFB through
// an exact integer box resolve and never touches PSPresentBlit. Measured on a
// Final Haunt burst frame at internal scale 3, isolated single-pixel outliers
// were 3 on the screenshot and 813 on the presented 1440x1080 image.
//
// This mirrors GxBuildAxisTaps from kGxPresentShader exactly - same closed-form
// pair count, same overlap weights, same bilinear tap position - and asserts
// the taps reconstruct a TRUE box average of the source. Any drift between this
// and the HLSL shows up as a failing ratio here.
struct PresentAxisTaps {
    static constexpr int kMaxTaps = 8;
    double uv[kMaxTaps]{};
    double weight[kMaxTaps]{};
    int count = 1;
};

PresentAxisTaps build_present_axis_taps(double centre, double footprint, double dim,
                                        int limit) {
    PresentAxisTaps taps;
    taps.uv[0] = centre;
    taps.weight[0] = 1.0;
    if (footprint * dim <= 1.0 || limit <= 1) {
        return taps;
    }
    const double low = (centre - footprint * 0.5) * dim;
    const double high = (centre + footprint * 0.5) * dim;
    const int first = static_cast<int>(std::floor(low));
    const int last = static_cast<int>(std::ceil(high)) - 1;
    const int pairs = std::min((last - first) / 2 + 1, limit);
    for (int step = 0; step < PresentAxisTaps::kMaxTaps; ++step) {
        const int texel = first + step * 2;
        const double w0 = std::max(std::min(high, texel + 1.0) - std::max(low, double(texel)), 0.0);
        const double w1 =
            std::max(std::min(high, texel + 2.0) - std::max(low, texel + 1.0), 0.0);
        const double total = w0 + w1;
        taps.uv[step] = total > 0.0 ? (texel + 0.5 + w1 / total) / dim : centre;
        taps.weight[step] = step < pairs ? total : 0.0;
    }
    taps.count = std::max(pairs, 1);
    return taps;
}

void test_present_area_resolve_is_an_exact_box() {
    // A source row whose values vary sharply, so any misweighting shows up.
    const int source_width = 96;
    std::vector<double> source(source_width);
    for (int index = 0; index < source_width; ++index) {
        source[index] = 20.0 + 90.0 * ((index * 7) % 11) + ((index % 5) == 0 ? 140.0 : 0.0);
    }
    const auto sample_bilinear = [&](double uv) {
        const double position = uv * source_width - 0.5;
        const int base = static_cast<int>(std::floor(position));
        const double fraction = position - base;
        const auto at = [&](int i) { return source[std::clamp(i, 0, source_width - 1)]; };
        return at(base) * (1.0 - fraction) + at(base + 1) * fraction;
    };
    // Every ratio the presenter can actually produce, including 1.333 - internal
    // scale 3 into a 1080p window - which the pre-fix heuristic resolved with a
    // single centred bilinear tap.
    const double ratios[] = {1.333333333, 1.5, 2.0, 2.666666667, 3.0, 3.84, 6.0};
    for (const double ratio : ratios) {
        const int destination = static_cast<int>(source_width / ratio);
        double worst = 0.0;
        for (int index = 0; index < destination; ++index) {
            const double centre = (index + 0.5) / destination;
            const double footprint = 1.0 / destination;
            const PresentAxisTaps taps =
                build_present_axis_taps(centre, footprint, source_width, 8);
            double sum = 0.0;
            double total = 0.0;
            for (int tap = 0; tap < taps.count; ++tap) {
                sum += sample_bilinear(taps.uv[tap]) * taps.weight[tap];
                total += taps.weight[tap];
            }
            require(total > 0.0, "present area resolve produced no weight");
            const double resolved = sum / total;
            // Reference: the true average of the source over the footprint.
            const double low = (centre - footprint * 0.5) * source_width;
            const double high = (centre + footprint * 0.5) * source_width;
            double reference = 0.0;
            for (int texel = static_cast<int>(std::floor(low));
                 texel < static_cast<int>(std::ceil(high)); ++texel) {
                const double overlap =
                    std::max(std::min(high, texel + 1.0) - std::max(low, double(texel)), 0.0);
                reference += overlap * source[std::clamp(texel, 0, source_width - 1)];
            }
            reference /= (high - low);
            worst = std::max(worst, std::abs(resolved - reference));
        }
        require(worst < 1e-9, "present area resolve is not an exact box at ratio " +
                                  std::to_string(ratio) + ", worst error " +
                                  std::to_string(worst));
    }
    // Magnification and the A/B arm must both stay the plain single bilinear
    // tap, which is what keeps internal scale 1 unchanged by construction.
    const PresentAxisTaps magnifying = build_present_axis_taps(0.5, 1.0 / 512.0, 64.0, 8);
    require(magnifying.count == 1 && magnifying.weight[0] == 1.0 && magnifying.uv[0] == 0.5,
            "magnification must take the plain bilinear sample");
    const PresentAxisTaps disabled = build_present_axis_taps(0.5, 1.0 / 32.0, 128.0, 1);
    require(disabled.count == 1 && disabled.weight[0] == 1.0 && disabled.uv[0] == 0.5,
            "gxPresentMaxTaps == 1 must reproduce the single-tap blit");
}

struct TestCase {
    const char* name;
    void (*run)();
};

const TestCase kTests[] = {
    {"display aspect and Hor+", test_display_aspect_and_horplus},
    {"display HUD anchors and scale", test_display_hud_anchors_and_scale},
    {"display widescreen layout invariants", test_display_widescreen_layout_invariants},
    {"display group coverage and anchors", test_display_group_coverage_and_anchors},
    {"display fit and config parse", test_display_fit_and_config_parse},
    {"display auto IR and fitted HUD inset", test_display_internal_scale_and_fitted_hud_inset},
    {"display bilinear scale bit-identical", test_display_scale_bilinear_matches_reference},
    {"render geometry internal-scale copy contract", test_render_geometry_internal_scale_copy_contract},
    {"recompiled code refuses entries below the image", test_recompiled_code_refuses_entries_below_the_image},
    {"verify-recomp per-call granularity", test_verify_recomp_per_call_granularity},
    {"verify-recomp leaves the game clock alone", test_verify_recomp_leaves_the_game_clock_alone},
    {"verify-recomp mismatch keeps the reference and stops", test_verify_recomp_mismatch_keeps_the_reference_and_stops},
    {"verify-recomp unrecognisable end is skipped", test_verify_recomp_unrecognisable_end_is_skipped},
    {"verify-recomp interpreter-only device access", test_verify_recomp_reports_interpreter_only_device_access},
    {"verify-recomp checks every interpreter-written byte", test_verify_recomp_checks_every_interpreter_written_byte},
    {"verify-recomp compares the lwarx reservation", test_verify_recomp_compares_reservation},
    {"verify-recomp runs HLE handlers once", test_verify_recomp_runs_hle_handlers_once},
    {"verify-recomp covers mid-function dispatch", test_verify_recomp_covers_mid_function_dispatch},
    {"verify-recomp segment overheads", test_verify_recomp_segment_overheads},
    {"verify-recomp keeps the native interrupt line", test_verify_recomp_keeps_the_native_interrupt_line},
    {"interpreter lswi/lswx register extent", test_string_load_register_extent},
    {"FP-unavailable shared interpreter/verifier semantics", test_fp_unavailable_is_shared_by_interpreter_and_verifier},
    {"HID2 illegal-instruction shared interpreter/verifier semantics", test_illegal_extension_is_shared_by_interpreter_and_verifier},
    {"write journal traces each guest access once", test_write_journal_traces_each_guest_access_once},
    {"host jobs deterministic barrier", test_host_jobs_partition_and_join_deterministically},
    {"unsigned divide native intrinsic parity", test_unsigned_divide_intrinsic_matches_recovered_helper},
    {"signed divide native 64-bit intrinsic full-state parity", test_signed_divide_intrinsic_matches_recovered_helper},
    {"unsigned divide native 64-bit intrinsic full-state parity", test_unsigned_divide_intrinsic_matches_recovered_helper_full_state},
    {"divide intrinsics serve every nonzero divide", test_divide_intrinsics_serve_every_nonzero_divide},
    {"HLE serves identically in both engines", test_hle_serves_identically_in_both_engines},
    {"frC probe classifies single multiply operands", test_frc_probe_classifies_single_multiply_operands},
    {"exception restores user fprs on rfi", test_exception_restores_user_fprs_on_rfi},
    {"cpu savestate preserves nested interrupt fpr context", test_cpu_savestate_preserves_nested_interrupt_fpr_context},
    {"cpu savestate carries the lwarx reservation", test_cpu_savestate_carries_reservation},
    {"cpu savestate carries the game clock", test_cpu_savestate_carries_the_game_clock},
    {"cpu reset matches fresh construction", test_cpu_reset_matches_fresh_construction},
    {"decrementer delivery: off by the boot, on by mtdec, named stop", test_decrementer_delivery_off_by_boot_on_by_mtdec},
    {"decrementer condition is delayed, not discarded", test_decrementer_condition_is_delayed_not_discarded},
    {"deliverable interrupt reaches every native poll", test_deliverable_interrupt_reaches_every_native_poll},
    {"wait point event reaches the back-edge poll", test_wait_point_event_reaches_the_back_edge_poll},
    {"unmapped access halts at the first fault", test_unmapped_access_halts_at_first_fault},
    {"host diagnostic peeks have no side effects", test_host_peeks_have_no_side_effects},
    {"bus translation listeners are per cpu", test_translation_listeners_are_per_cpu},
    {"try_hle filter is exact for any hook count", test_try_hle_filter_is_exact_for_any_hook_count},
    {"condition register encoding", test_condition_register_encoding},
    {"system call return address", test_system_call_return_address},
    {"rotate and mask", test_rotate_and_mask},
    {"carry and overflow", test_carry_and_overflow},
    {"paired single quantised load", test_paired_single_quantised_load},
    {"translation is the window", test_translation_is_the_window},
    {"window page table fast path", test_window_page_table_fast_path},
    {"game clock delivers events in order", test_game_clock_delivers_events_in_order},
    {"wait point advances one event at a time", test_wait_point_advances_one_event_at_a_time},
    {"poll point waits only at a fixed point", test_poll_point_waits_only_at_a_fixed_point},
    {"host thread stop reaches a spinning guest", test_host_thread_stop_reaches_a_spinning_guest},
    {"reset drops a posted host stop", test_reset_drops_a_posted_host_stop},
    {"native video field clock", test_native_video_field_clock},
    {"native interrupts follow the OS priority", test_native_interrupts_follow_os_priority},
    {"native video platform sites", test_native_video_platform_sites},
    {"native video savestate round trip", test_native_video_savestate_round_trip},
    {"native GX FIFO binding", test_native_gx_fifo_binding},
    {"native GX breakpoint raise and disable", test_native_gx_breakpoint_raise_and_disable},
    {"native GX draw-sync token and draw-done", test_native_gx_draw_sync_token_and_draw_done},
    {"native GX draw-sync callback is refused", test_native_gx_draw_sync_callback_is_refused},
    {"native frame limiter paces events at one period", test_native_frame_limiter_paces_events_at_one_period},
    {"native object range keeps the original boundary", test_native_object_range_keeps_the_original_boundary},
    {"appear fade only for models appearing in view", test_appear_fade_only_for_models_appearing_in_view},
    {"vertex format strides", test_vertex_format_strides},
    {"native immediate builds primitives", test_native_immediate_builds_primitives},
    {"native immediate fails closed", test_native_immediate_fails_closed},
    {"native immediate savestate mid-primitive", test_native_immediate_savestate_mid_primitive},
    {"cpu GX sites in the interpreter", test_cpu_gx_sites_in_the_interpreter},
    {"cpu platform sites in the interpreter", test_cpu_platform_sites_in_the_interpreter},
    {"verify-recomp GX sites", test_verify_recomp_gx_sites},
    {"native GX shutdown settles then aborts", test_native_gx_shutdown_settles_then_aborts},
    {"native GX sync savestate and deleted registers", test_native_gx_sync_savestate_and_deleted_registers},
    {"asset read contract and failures", test_asset_read_contract_and_failures},
    {"AX voice end and loop windows", test_ax_voice_end_and_loop_windows},
    {"native DSP DMA completion", test_native_dsp_dma_completion},
    {"native AI pass timing", test_native_ai_pass_timing},
    {"native interrupt line follows status and mask", test_native_interrupt_line_follows_status_and_mask},
    {"system load dol preserves data inside bss header", test_system_load_dol_preserves_data_inside_bss_header},
    {"savestate invalidates unrestored motion blur history", test_savestate_invalidates_unrestored_motion_blur_history},
    {"texel footprint regimes", test_texel_footprint_regimes},
    {"present area resolve is an exact box", test_present_area_resolve_is_an_exact_box},
    {"native card store policy", test_native_card_store_policy},
    {"native card created cards and refused sizes", test_native_card_created_cards},
    {"native card mount, probe and unmount", test_native_card_mount_probe_unmount},
    {"native IPL services: SRAM and UART", test_native_ipl_sram_and_uart},
    {"native card completion rank", test_native_card_completion_rank},
    {"native __OSInterruptInit", test_native_interrupt_init},
    {"native reset controls", test_native_reset_controls},
    {"native disc-error module", test_native_disc_error_module},
    {"native refusing interrupt handlers", test_native_refusing_interrupt_handlers},
    {"unserved guest access stops", test_unserved_guest_access_stops},
};

}  // namespace

int main() {
    std::cout << std::unitbuf;  // a crash still leaves the last test's line behind
    int failures = 0;
    for (const TestCase& test : kTests) {
        try {
            test.run();
            std::cout << "PASS " << test.name << "\n";
        } catch (const std::exception& error) {
            std::cout << "FAIL " << test.name << ": " << error.what() << "\n";
            ++failures;
        }
    }
    std::cout << (failures == 0 ? "all gekko core tests passed\n" : "gekko core tests failed\n");
    return failures == 0 ? 0 : 1;
}
