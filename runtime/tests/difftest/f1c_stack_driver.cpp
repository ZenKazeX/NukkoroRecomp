// F1c host-stack integrity driver (test-only).
//
// Links the F1c generated objects (real tools/ppcrecomp output for tools/difftest/gen_f1c.py's
// synthetic DOL, compiled with the shadow_recomp shard flags) and the PRODUCTION shadow_runtime.
// Guest code is entered only through Cpu::run() - the only entry shadow_main uses; native time has no
// instruction budget - with the generated table attached.
//
// Each job runs one loop shape on a fresh thread whose stack reserve is large enough to MEASURE a
// growing host stack instead of dying of it (the production game thread has 16 MiB), all on the same thread:
//   0. calibration: three runs that stop at once (entry = the untranslated sentinel: the dispatcher and the
//      stop path every shape run ends with); the smallest painted depth is the job's `painted_baseline`
//   1. warm-up run:  `warm` iterations (absorbs first-use paths); probe watermark D0
//   2. quarter run:  `iterations` / 4 iterations (N); painted depth P1
//   3. main run:     `iterations` iterations (4N); probe watermark D1, painted depth P4, committed stack C1
// Every run is measured by STACK PAINTING (all samplers, every shape): the kPaintBytes below the run's frame
// are filled with a pattern right before Cpu::run and the lowest word the run overwrote gives the deepest host
// stack it reached (difftest_driver.cpp's method). Painting needs no host call inside the loop, so it measures
// the shapes the probe sampler cannot (below). The record reports, per run, "painted_depth" (bytes from the
// run's frame to that word, the same reference the probe sampler uses) and "painted_saturated" (the whole
// window was overwritten: the depth is at least the window). run_f1c_stack.py decides: a shape is BOUNDED
// when P4 == P1 within a small fixed slack (the host stack does not grow with guest iterations), P4 less the
// calibrated baseline stays within the host-stack bound, no run saturates the window, the committed stack
// (measured after painting, so it sees growth beyond the window) grows by at most 8 KiB in the main run, and
// - where the probe sampler reaches a probe - D1 == D0.
//
// Samplers (native time, docs/native-time-20261005/DESIGN.md: the runtime has no per-block hook any more -
// retire_block and Cpu::set_hardware_tick are deleted, and guest code runs in zero game time with no
// service inside it):
//   hook   the host stack is sampled by a DECLINING HLE probe at every translated record head the loop
//          enters on each iteration other than the shape's own entry (a direct call, a published LK=0
//          transfer and an indirect call all enter a record head through its entry thunk, whose try_hle
//          runs the probe), at the shared translated leaf (f1c::kLeaf) and inside the shape's HLE-served
//          callee. The probe runs on one fixed host path (entry thunk -> try_hle_slow -> handler), declines,
//          and the translated body runs unchanged, so it samples what the retire_block tick sampled: the
//          host stack depth inside the native call tree, plus the depth checkpoints (iteration = r3) and
//          the backtrace at iteration 1,000. `tick1` - the name run_f1c_stack.py passes - selects this
//          sampler. A run whose loop reached no probe has nothing to sample - a loop the generator compiled
//          into one native body that makes no host call (today the tiny intra_t, xrec_direct_t, eof_fall_t
//          and bctr_local_t) or one that returns to Cpu::run every lap (bclr_top_t) - and reports status
//          "no-sample-point" instead of a vacuous 0 == 0 watermark: such a shape creates no host frame
//          inside its loop, and the painted depth (above) is what measures it; run_f1c_stack.py accepts that
//          status only for the shapes gen_f1c.py builds without a host call per iteration.
//   none   no probe is installed (the shipped configuration); the painted depth and the committed-stack
//          measurement (NT_TIB StackLimit only moves down) apply.
// Every run must end where the production dispatcher stops at the untranslated sentinel (ExecutionFault
// "native execution cannot continue at 0x80009000"), with r3 == the iteration count and r1 == the original
// stack pointer (the shape returned through its own frames).
//
// usage: f1c_stack_driver --shape <name|all> --ee <0|1> [--iterations N] [--warm N (< N/4)]
//                         [--sampler hook|tick1|none] [--reserve-mib M] --out results.jsonl
//        f1c_stack_driver --state N --shape <name|all> --ee <0|1> [--engine native|interp] --out states.jsonl
//   --state: run each shape once for N iterations and write its final ARCHITECTED state (GPR, FPR, CR, XER, LR,
//   CTR, MSR, FPSCR, pc, every SPR, SR, time base, reservation, interrupt lines, fault latch, hashes of all of
//   MEM1, ARAM and the locked cache) for the guest-state cross-check. `--engine interp` exists only in the
//   reference flavour (f1c_stack_driver_ref, shadow_runtime_reference): the interpreter steps the same guest
//   program with no translated code attached. Native code has no instruction count, so the two engines are
//   compared at one boundary both find without a count - the shape's return to the untranslated sentinel with
//   the original stack pointer - on everything the guest can observe; the loop counters (r3 = N, r4/r7) and
//   the data window carry what the retired count used to. The interpreter's own count is written outside
//   "state" ("interpreted"), never compared.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/recomp.h"

#include "f1c_fixture.h"
#include "f1c_host.h"

#include <intrin.h>
#include <malloc.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace gekko;

namespace {

constexpr u64 kCheckpoints[] = {1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u, 10000000u};
constexpr std::size_t kCheckpointCount = sizeof(kCheckpoints) / sizeof(kCheckpoints[0]);
// Stack painting window below a run's frame, and its fill word. A bounded shape reaches a few KiB; an unbounded
// one (one host frame kept per iteration) overwrites the whole window within a few thousand iterations.
constexpr std::size_t kPaintBytes = 256u * 1024u;
constexpr u64 kPaint = 0xD1FF7E57C0DE5EEDull;
constexpr int kCalibrationRuns = 3;

struct Measure {
    u64 samples = 0;
    std::uintptr_t base = 0;
    std::uintptr_t max_depth = 0;
    std::uintptr_t cp_max[kCheckpointCount]{};
    std::uintptr_t cp_now[kCheckpointCount]{};
    bool cp_seen[kCheckpointCount]{};
    // Host return addresses (executable RVAs) at the first sample past iteration kBacktraceAt: a growing
    // stack shows the per-iteration cycle of CALL sites that keep their frames.
    std::uintptr_t backtrace[62]{};
    std::size_t backtrace_frames = 0;
};
constexpr u64 kBacktraceAt = 1000u;

struct RunResult {
    std::string status = "ok";
    u32 pc = 0;
    u32 r3 = 0;
    u64 dispatches = 0;
    u64 yields = 0;
    u64 interpreter_exits = 0;
    u64 hle_hits = 0;
    int stop_reason = 0;
    std::string stop_detail;
    Measure m;
    std::uintptr_t commit_before = 0;
    std::uintptr_t commit_after = 0;
    double wall_ms = 0.0;
    u64 painted_depth = 0;           // bytes from m.base down to the deepest word the run overwrote
    bool painted_saturated = false;  // the lowest painted word was overwritten: the depth is >= the window
};

struct Job {
    const f1c::Shape* shape = nullptr;
    bool ee = false;
    bool probe = true;          // the hook sampler (`hook` / `tick1`)
    std::vector<u32> probes;    // its probe addresses (probe_addresses)
    u64 warm = 1000;
    u64 iterations = 1000000;
    RecompiledCode* code = nullptr;
    RunResult calibration[kCalibrationRuns];
    RunResult warm_result;
    RunResult quarter_result;   // iterations / 4 (N); the main run is 4N
    RunResult main_result;
    bool guard_before = false;
    bool guard_after = false;
};

std::string quote(const std::string& text) {
    std::string out = "\"";
    for (char ch : text) {
        if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
        else if (static_cast<unsigned char>(ch) < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", ch); out += b; }
        else out += ch;
    }
    return out + "\"";
}

// The address of this call's return-address slot: one fixed offset below the caller's frame.
GEKKO_NOINLINE std::uintptr_t probe_address() {
    return reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
}

// Fills kPaintBytes of stack below the caller's frame (_alloca probes page by page, so the guard page below the
// committed stack is touched in order) and returns the lowest painted address. Everything the caller calls next
// - Cpu::run and the whole native call tree under it - lands in the painted window.
GEKKO_NOINLINE std::uintptr_t paint_stack() {
    auto* region = static_cast<volatile u64*>(_alloca(kPaintBytes));
    for (std::size_t i = 0; i < kPaintBytes / sizeof(u64); ++i) region[i] = kPaint;
    return reinterpret_cast<std::uintptr_t>(region);
}

// The lowest painted word something overwrote since paint_stack (the region top if nothing did).
GEKKO_NOINLINE std::uintptr_t deepest_overwrite(std::uintptr_t bottom) {
    const auto* region = reinterpret_cast<const volatile u64*>(bottom);
    std::size_t i = 0;
    while (i < kPaintBytes / sizeof(u64) && region[i] == kPaint) ++i;
    return bottom + i * sizeof(u64);
}

// The translated record heads of shape `s` other than its entry: the generator names every record
// "<shape>_<k>_<ADDRESS>" (tools/difftest/gen_f1c.py rows + ppcrecomp), and only a record head - never an
// interior entry point, which carries its record's name - has its own address as the name's suffix. Every
// record head has an entry thunk that calls try_hle; interior entry thunks do not.
std::vector<u32> probe_addresses(const f1c::Shape& s) {
    std::vector<u32> out{f1c::kLeaf};
    const std::string prefix = std::string(s.name) + "_";
    for (std::size_t i = 0; i < recomp::kFunctionCount; ++i) {
        const RecompiledCode::Entry& e = recomp::kFunctions[i];
        if (e.address == s.entry || e.address == s.hook || e.address == f1c::kLeaf || e.name == nullptr) continue;
        const std::string name = e.name;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "_%08X", e.address);
        const std::size_t length = std::strlen(suffix);
        if (name.size() <= length || name.compare(name.size() - length, length, suffix) != 0) continue;
        if (std::find(out.begin(), out.end(), e.address) == out.end()) out.push_back(e.address);
    }
    return out;
}

void sample(Measure& m, const Cpu& c) {
    const std::uintptr_t here = probe_address();
    const std::uintptr_t depth = m.base > here ? m.base - here : 0u;
    ++m.samples;
    m.max_depth = std::max(m.max_depth, depth);
    const u64 iteration = c.gpr[3];
    if (m.backtrace_frames == 0u && iteration >= kBacktraceAt) {
        m.backtrace_frames = f1c::capture_backtrace_rvas(m.backtrace, 62u);
    }
    for (std::size_t k = 0; k < kCheckpointCount; ++k) {
        if (!m.cp_seen[k] && iteration >= kCheckpoints[k]) {
            m.cp_seen[k] = true;
            m.cp_now[k] = depth;
            m.cp_max[k] = m.max_depth;
        }
    }
}

const char* sentinel_stop_prefix() {
    static char expected[96];
    std::snprintf(expected, sizeof(expected), "native execution cannot continue at 0x%08X", f1c::kSentinel);
    return expected;
}

// One run of the job's shape for `iterations` iterations, or (calibration) a run entered at the untranslated
// sentinel, which the dispatcher stops at once through the same stop path every shape run ends with. Both kinds
// go through this one function so the painted depths share one frame layout and are directly comparable.
void run_shape(Job& job, u64 iterations, RunResult& r, bool calibration = false) {
    const f1c::Shape& s = *job.shape;
    auto bus = std::make_unique<GuestMemory>();
    auto cpu = std::make_unique<Cpu>(*bus);
    Cpu& c = *cpu;
    for (const f1c::CodeRow& row : f1c::kRows) {
        for (std::size_t i = 0; i < row.count; ++i) bus->write32(row.address + static_cast<u32>(4u * i), row.words[i]);
    }
    c.reset();
    c.attach_recompiled(job.code);
    Measure& m = r.m;
    u64 hle_hits = 0;
    const bool sampled = job.probe;
    if (s.hook != 0u) {
        c.install_hle(s.hook, [&hle_hits, &m, sampled](Cpu& cpu) {
            ++hle_hits;
            if (sampled) sample(m, cpu);
            cpu.gpr[7] += 1u;  // the translated leaf's addi r7,r7,1; pc <- lr is try_hle_slow's
            return true;
        }, "f1c served callee");
    }
    if (sampled) {
        for (const u32 address : job.probes) {
            c.install_hle(address, [&m](Cpu& cpu) {
                sample(m, cpu);
                return false;  // declined: the translated record runs unchanged
            }, "f1c stack probe");
        }
    }
    for (u32 i = 0; i < 32u; ++i) c.gpr[i] = 0u;
    c.gpr[1] = f1c::kStackTop;
    c.gpr[5] = static_cast<u32>(iterations);
    c.gpr[6] = f1c::kDataBase;
    for (u32 k = 0; k < 8u; ++k) bus->write32(f1c::kDataBase + 4u * k, s.table[k]);
    c.lr = f1c::kSentinel;
    if (job.ee) c.set_msr(c.msr() | (kMsrEE)); else c.set_msr(c.msr() & (~kMsrEE));
    // Arms the decrementer edge 2^31 game ticks out; guest code never moves the game clock, so no edge falls
    // in any run here.
    c.set_decrementer(0x7FFFFFFFu);
    c.set_entry(calibration ? f1c::kSentinel : s.entry);

    m.base = probe_address();
    // Paint, then measure the committed stack: the commit check sees growth beyond the painted window, the
    // painted depth everything inside it.
    const std::uintptr_t paint_bottom = paint_stack();
    r.commit_before = f1c::stack_base() - f1c::stack_limit();
    const double t0 = f1c::now_ms();
    // The loop terminates by returning to the untranslated sentinel, where the production dispatcher stops
    // with ExecutionFault ("native execution cannot continue").
    c.run();
    r.wall_ms = f1c::now_ms() - t0;
    r.commit_after = f1c::stack_base() - f1c::stack_limit();
    const std::uintptr_t deepest = deepest_overwrite(paint_bottom);
    r.painted_depth = m.base > deepest ? static_cast<u64>(m.base - deepest) : 0u;
    r.painted_saturated = deepest == paint_bottom;
    r.pc = c.pc;
    r.r3 = c.gpr[3];
    r.dispatches = c.recompiled_calls;
    r.yields = c.fallback_total(FallbackReason::Yield);
    r.interpreter_exits = c.interpreter_exits;
    r.hle_hits = hle_hits;
    r.stop_reason = static_cast<int>(c.stop().reason);
    r.stop_detail = c.stop().detail;
    if (c.stop().reason != StopReason::ExecutionFault || c.stop().detail.rfind(sentinel_stop_prefix(), 0) != 0 ||
        c.pc != f1c::kSentinel) {
        r.status = "unexpected-stop";
    } else if (calibration) {
        // Entered at the sentinel: nothing ran, so there is no iteration count, frame or probe to check.
    } else if (c.gpr[3] != static_cast<u32>(iterations)) {
        r.status = "wrong-iteration-count";
    } else if (c.gpr[1] != f1c::kStackTop) {
        r.status = "stack-pointer-not-restored";
    } else if (sampled && m.samples == 0u) {
        r.status = "no-sample-point";
    }
}

u64 fnv_bytes(const u8* p, std::size_t n) {
    u64 h = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

// One run of `iterations` for the guest-state cross-check (no host-stack measurement).
std::string state_run(const f1c::Shape& s, bool ee, u64 iterations, bool interp, RecompiledCode* code) {
    auto bus = std::make_unique<GuestMemory>();
    auto cpu = std::make_unique<Cpu>(*bus);
    Cpu& c = *cpu;
    for (const f1c::CodeRow& row : f1c::kRows) {
        for (std::size_t i = 0; i < row.count; ++i) bus->write32(row.address + static_cast<u32>(4u * i), row.words[i]);
    }
    c.reset();
    c.attach_recompiled(interp ? nullptr : code);
    if (s.hook != 0u) {
        c.install_hle(s.hook, [](Cpu& cpu) { cpu.gpr[7] += 1u; return true; }, "f1c served callee");
    }
    for (u32 i = 0; i < 32u; ++i) c.gpr[i] = 0u;
    c.gpr[1] = f1c::kStackTop;
    c.gpr[5] = static_cast<u32>(iterations);
    c.gpr[6] = f1c::kDataBase;
    for (u32 k = 0; k < 8u; ++k) bus->write32(f1c::kDataBase + 4u * k, s.table[k]);
    c.lr = f1c::kSentinel;
    if (ee) c.set_msr(c.msr() | (kMsrEE)); else c.set_msr(c.msr() & (~kMsrEE));
    c.set_decrementer(0x7FFFFFFFu);
    c.set_entry(s.entry);
    std::string status = "ok";
    if (interp) {
#if defined(DIFFTEST_REFERENCE)
        // The boundary: the first arrival at the untranslated sentinel (the shape's final return).
        u64 steps = 0;
        while (c.pc != f1c::kSentinel && !c.stopped()) {
            c.step();
            if (++steps > 4000000000ull) { status = "step-limit"; break; }
        }
        if (c.stopped()) status = "interpreter-stopped";
#else
        status = "no-interpreter-in-production-build";
#endif
    } else {
        c.run();
#if defined(DIFFTEST_REFERENCE)
        // The reference dispatcher interprets the sentinel instead of stopping; it is not translated code.
        status = "native-lane-not-supported-in-reference-flavour";
#else
        if (c.stop().reason != StopReason::ExecutionFault || c.stop().detail.rfind(sentinel_stop_prefix(), 0) != 0 ||
            c.pc != f1c::kSentinel)
            status = "unexpected-stop";
#endif
    }
    // Both engines must have reached the same boundary: the loop ran to completion and the shape returned
    // through its own frames.
    if (status == "ok" && c.gpr[3] != static_cast<u32>(iterations)) status = "wrong-iteration-count";
    if (status == "ok" && c.gpr[1] != f1c::kStackTop) status = "stack-pointer-not-restored";
    char b[768];
    std::string j = "{\"shape\":\"" + std::string(s.name) + "\",\"ee\":" + (ee ? "1" : "0") + ",\"engine\":\"" +
                    (interp ? "interp" : "native") + "\",\"status\":\"" + status + "\",\"interpreted\":" +
                    std::to_string(c.interpreted_instructions()) + ",\"state\":{\"gpr\":[";
    for (int i = 0; i < 32; ++i) { std::snprintf(b, sizeof(b), "%s\"0x%08X\"", i ? "," : "", c.gpr[i]); j += b; }
    j += "],\"fpr\":[";
    for (int i = 0; i < 32; ++i) {
        std::snprintf(b, sizeof(b), "%s\"0x%016llX:0x%016llX\"", i ? "," : "", static_cast<unsigned long long>(c.fpr[i].bits[0]),
                      static_cast<unsigned long long>(c.fpr[i].bits[1]));
        j += b;
    }
    // No "retired": native code has no instruction count. The time base is the game clock's view, which guest
    // code never moves, so both engines must still report the value it had at entry.
    std::snprintf(b, sizeof(b), "],\"cr\":\"0x%08X\",\"xer\":\"0x%08X\",\"lr\":\"0x%08X\",\"ctr\":\"0x%08X\",\"msr\":\"0x%08X\","
                                "\"fpscr\":\"0x%08X\",\"pc\":\"0x%08X\",\"tb\":%llu,\"reserve\":%d,\"reserve_addr\":\"0x%08X\","
                                "\"line_ext\":%d,\"line_dec\":%d,\"fault\":%d",
                  c.cr, c.xer, c.lr, c.ctr, c.msr(), c.fpscr, c.pc, static_cast<unsigned long long>(c.time_base()),
                  c.reservation ? 1 : 0, c.reservation_address,
                  c.external_interrupt() ? 1 : 0, c.decrementer_pending() ? 1 : 0, c.memory_fault().latched ? 1 : 0);
    j += b;
    j += ",\"spr\":[";
    bool first = true;
    for (u32 i = 0; i < 1024u; ++i) {
        // The decrementer is Cpu::decrementer(), not its spr[] slot (C1).
        const u32 value = i == SPR_DEC ? c.decrementer() : c.spr[i];
        if (value == 0u) continue;
        std::snprintf(b, sizeof(b), "%s[%u,\"0x%08X\"]", first ? "" : ",", i, value);
        j += b;
        first = false;
    }
    j += "],\"sr\":[";
    for (int i = 0; i < 16; ++i) { std::snprintf(b, sizeof(b), "%s\"0x%08X\"", i ? "," : "", c.sr[i]); j += b; }
    // Every mapped region's committed bytes (the program image, the OS heap region, the locked cache).
    u64 digest = 0xCBF29CE484222325ull;
    for (const auto& region : bus->regions()) {
        const u8* bytes = bus->peek_pointer(region.base, region.committed);
        for (std::size_t i = 0; bytes != nullptr && i < region.committed; ++i) {
            digest ^= bytes[i];
            digest *= 0x100000001B3ull;
        }
    }
    std::snprintf(b, sizeof(b), "],\"memory\":\"%016llx\"}}", static_cast<unsigned long long>(digest));
    return j + b;
}

void job_thread(void* arg) {
    Job& job = *static_cast<Job*>(arg);
    job.guard_before = f1c::guard_page_below_limit();
    for (RunResult& calibration : job.calibration) run_shape(job, 0u, calibration, true);
    run_shape(job, job.warm, job.warm_result);
    run_shape(job, job.iterations / 4u, job.quarter_result);
    run_shape(job, job.iterations, job.main_result);
    job.guard_after = f1c::guard_page_below_limit();
}

std::string result_json(const RunResult& r, bool sampled) {
    std::string j = "{\"status\":" + quote(r.status);
    char b[512];
    // No "retired": native time has no instruction count.
    std::snprintf(b, sizeof(b),
                  ",\"pc\":\"0x%08X\",\"r3\":%u,\"dispatches\":%llu,\"yields\":%llu,"
                  "\"interpreter_exits\":%llu,\"hle_hits\":%llu,\"stop_reason\":%d,\"samples\":%llu,"
                  "\"max_depth\":%llu,\"commit_before\":%llu,\"commit_after\":%llu,\"wall_ms\":%.1f,"
                  "\"painted_depth\":%llu,\"painted_saturated\":%s",
                  r.pc, r.r3, static_cast<unsigned long long>(r.dispatches),
                  static_cast<unsigned long long>(r.yields), static_cast<unsigned long long>(r.interpreter_exits),
                  static_cast<unsigned long long>(r.hle_hits), r.stop_reason,
                  static_cast<unsigned long long>(r.m.samples), static_cast<unsigned long long>(r.m.max_depth),
                  static_cast<unsigned long long>(r.commit_before), static_cast<unsigned long long>(r.commit_after),
                  r.wall_ms, static_cast<unsigned long long>(r.painted_depth), r.painted_saturated ? "true" : "false");
    j += b;
    j += ",\"stop_detail\":" + quote(r.stop_detail);
    j += ",\"checkpoints\":[";
    bool first = true;
    if (sampled) {
        for (std::size_t k = 0; k < kCheckpointCount; ++k) {
            if (!r.m.cp_seen[k]) continue;
            if (!first) j += ',';
            first = false;
            std::snprintf(b, sizeof(b), "[%llu,%llu,%llu]", static_cast<unsigned long long>(kCheckpoints[k]),
                          static_cast<unsigned long long>(r.m.cp_now[k]), static_cast<unsigned long long>(r.m.cp_max[k]));
            j += b;
        }
    }
    j += "],\"backtrace_rva\":[";
    for (std::size_t k = 0; k < r.m.backtrace_frames; ++k) {
        if (k) j += ',';
        std::snprintf(b, sizeof(b), "\"0x%llx\"", static_cast<unsigned long long>(r.m.backtrace[k]));
        j += b;
    }
    return j + "]}";
}

}  // namespace

int main(int argc, char** argv) {
    std::string shape_arg = "all", out_path, sampler = "hook", engine = "native";
    int ee = -1;
    u64 state_iterations = 0;
    u64 iterations = 1000000u, warm = 1000u, reserve_mib = 1024u;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--shape") shape_arg = value();
        else if (a == "--ee") ee = std::atoi(value().c_str());
        else if (a == "--iterations") iterations = std::strtoull(value().c_str(), nullptr, 0);
        else if (a == "--warm") warm = std::strtoull(value().c_str(), nullptr, 0);
        else if (a == "--sampler") sampler = value();
        else if (a == "--reserve-mib") reserve_mib = std::strtoull(value().c_str(), nullptr, 0);
        else if (a == "--out") out_path = value();
        else if (a == "--state") state_iterations = std::strtoull(value().c_str(), nullptr, 0);
        else if (a == "--engine") engine = value();
        else if (a == "--list") {
            for (const f1c::Shape& s : f1c::kShapes) std::printf("%s\n", s.name);
            return 0;
        } else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    if (state_iterations != 0u) {
        if (out_path.empty() || (ee != 0 && ee != 1) || (engine != "native" && engine != "interp")) return 2;
        static RecompiledCode state_code;
        state_code.install(recomp::kFunctions, recomp::kFunctionCount);
        state_code.install_ranges(recomp::kRanges, recomp::kRangeCount);
        std::ofstream states(out_path, std::ios::binary | std::ios::app);
        for (const f1c::Shape& s : f1c::kShapes) {
            if (shape_arg != "all" && shape_arg != s.name) continue;
            states << state_run(s, ee != 0, state_iterations, engine == "interp", &state_code) << '\n';
        }
        return 0;
    }
    // `tick1` is the name run_f1c_stack.py passes; the retire_block tick it named no longer exists and the
    // per-iteration probe (`hook`) is its native replacement (see the header).
    if (sampler == "tick1") sampler = "hook";
    if (out_path.empty() || (ee != 0 && ee != 1) || (sampler != "hook" && sampler != "none") ||
        iterations == 0u || iterations > 0xFFFFFFFFull || warm == 0u || warm >= iterations / 4u) {
        std::fprintf(stderr, "usage: f1c_stack_driver --shape <name|all> --ee <0|1> [--iterations N] [--warm N] "
                             "[--sampler hook|tick1|none] [--reserve-mib M] --out results.jsonl\n");
        return 2;
    }
    static RecompiledCode code;
    code.install(recomp::kFunctions, recomp::kFunctionCount);
    code.install_ranges(recomp::kRanges, recomp::kRangeCount);

    std::ofstream out(out_path, std::ios::binary | std::ios::app);
    if (!out) { std::fprintf(stderr, "cannot write %s\n", out_path.c_str()); return 2; }
    int ran = 0;
    for (const f1c::Shape& s : f1c::kShapes) {
        if (shape_arg != "all" && shape_arg != s.name) continue;
        Job job;
        job.shape = &s;
        job.ee = ee != 0;
        job.probe = sampler == "hook";
        if (job.probe) job.probes = probe_addresses(s);
        job.warm = warm;
        job.iterations = iterations;
        job.code = &code;
        const unsigned long exit_code = f1c::run_on_thread(static_cast<std::size_t>(reserve_mib) << 20, &job_thread, &job);
        char head[320];
        std::snprintf(head, sizeof(head),
                      "{\"shape\":%s,\"control\":%s,\"ee\":%d,\"sampler\":%s,\"probe_points\":%zu,\"warm\":%llu,"
                      "\"iterations\":%llu,\"reserve_mib\":%llu,\"thread_exit\":\"0x%08lX\",\"guard_before\":%s,"
                      "\"guard_after\":%s",
                      quote(s.name).c_str(), s.control ? "true" : "false", ee, quote(sampler).c_str(),
                      job.probes.size() + (job.probe && s.hook != 0u ? 1u : 0u),
                      static_cast<unsigned long long>(warm), static_cast<unsigned long long>(iterations),
                      static_cast<unsigned long long>(reserve_mib), exit_code, job.guard_before ? "true" : "false",
                      job.guard_after ? "true" : "false");
        // The painted baseline: the shallowest calibration run that stopped as expected (first-use paths cannot
        // inflate it); null when none did.
        u64 baseline = ~0ull;
        std::string depths = "[", statuses = "[";
        for (int k = 0; k < kCalibrationRuns; ++k) {
            const RunResult& run = job.calibration[k];
            if (run.status == "ok" && !run.painted_saturated) baseline = std::min(baseline, run.painted_depth);
            depths += (k ? "," : "") + std::to_string(run.painted_depth);
            statuses += (k ? "," : "") + quote(run.status);
        }
        out << head << ",\"paint_window\":" << kPaintBytes << ",\"painted_baseline\":"
            << (baseline == ~0ull ? std::string("null") : std::to_string(baseline))
            << ",\"calibration_depths\":" << depths << "],\"calibration_status\":" << statuses << "]"
            << ",\"warm_run\":" << result_json(job.warm_result, job.probe)
            << ",\"quarter_run\":" << result_json(job.quarter_result, job.probe)
            << ",\"main_run\":" << result_json(job.main_result, job.probe) << "}\n";
        out.flush();
        ++ran;
    }
    if (ran == 0) { std::fprintf(stderr, "no shape named %s\n", shape_arg.c_str()); return 2; }
    return 0;
}
