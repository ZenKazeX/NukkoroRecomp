// F1c host-stack integrity on the GAME's own translated code (test-only).
//
// Links shadow_recomp (the production 64-shard translation of main.dol, exactly the objects the executable is
// linked from) and the production shadow_runtime, and runs two production guest cycles through Cpu::run()
// (verify-r2 host-stack-cross-record reproducers, turned into a measured, bounded regression job):
//
//   game_803D946C   the loop 0x803D948C..0x803D9540 spanning records fn_803D946C / fn_803D94AC (bctrl, record
//                   fall-through, cross-record `b` back edge). Guest RAM holds an object/vtable whose three
//                   virtual slots and the direct callee 0x803D8088 are HLE-served (r3 = 0: keep looping); the
//                   per-iteration callee 0x803CBF34 is the HLE-served probe. Both translated records run unmodified.
//   game_803A1AAC   the MSL pointer-to-member adjusters fn_803A1AAC / fn_803A1AD4 closed into a cycle through
//                   their bctr default paths (recomp::jump_indirect to a declared entry, no poll). Assist: a
//                   DECLINING hook on each entry loads r12 with the ptmf descriptor (standing in for the caller's
//                   `lwz r12`); the translated bodies run unmodified.
//
// Each job: 1,000 warm-up iterations, then `--iterations` on the same 1 GiB-reserve thread (fresh Bus/Cpu per
// run). The probe hook samples the host stack once per iteration; the committed stack (NT_TIB StackLimit) is
// measured around each run. Records use f1c_stack_driver's format (sampler "hook") so run_f1c_stack.py
// evaluates them with the same bounded-depth criterion.
//
// Native time (docs/native-time-20261005/DESIGN.md): the runtime counts no instructions, so a run is bounded and
// judged by what the guest and the probe observe, never by a retired count. Each run ends at the probe's
// HostRequest stop after exactly `n` probe iterations (`n` + 1 for game_803A1AAC, whose hook stops on the call
// after the last iteration); any other stop reason or iteration count fails the run. The record's "retired" field
// is written as null (no count exists; run_f1c_stack.py still reads the key). Neither cycle contains a wait
// point (extract/wait-points.txt), so no idle handler is needed: the game clock never moves here.
//
// usage: f1c_game_stack_driver --shape <game_803D946C|game_803A1AAC|all> --ee <0|1> [--iterations N]
//                              [--warm N] [--reserve-mib M] --out results.jsonl   |  --list
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/recomp.h"

#include "f1c_host.h"

#include <intrin.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>

using namespace gekko;

namespace {

constexpr u64 kCheckpoints[] = {1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u, 10000000u};
constexpr std::size_t kCheckpointCount = sizeof(kCheckpoints) / sizeof(kCheckpoints[0]);
constexpr u32 kReturn = 0x80009100u;  // untranslated

struct RunResult {
    std::string status = "ok";
    u64 iterations = 0;
    u64 dispatches = 0;
    int stop_reason = 0;
    std::string stop_detail;
    u32 pc = 0;
    std::uintptr_t base = 0, max_depth = 0;
    std::uintptr_t cp_now[kCheckpointCount]{}, cp_max[kCheckpointCount]{};
    bool cp_seen[kCheckpointCount]{};
    std::uintptr_t backtrace[62]{};
    std::size_t backtrace_frames = 0;
    std::uintptr_t commit_before = 0, commit_after = 0;
    double wall_ms = 0;
};

struct Job {
    std::string shape;
    bool ee = false;
    u64 warm = 1000, iterations = 1000000;
    RecompiledCode* code = nullptr;
    RunResult warm_result, main_result;
    bool guard_before = false, guard_after = false;
};

GEKKO_NOINLINE std::uintptr_t probe_address() {
    return reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
}

void sample(RunResult& r, u64 iteration) {
    const std::uintptr_t here = probe_address();
    const std::uintptr_t depth = r.base > here ? r.base - here : 0u;
    r.max_depth = std::max(r.max_depth, depth);
    for (std::size_t k = 0; k < kCheckpointCount; ++k) {
        if (!r.cp_seen[k] && iteration >= kCheckpoints[k]) {
            r.cp_seen[k] = true;
            r.cp_now[k] = depth;
            r.cp_max[k] = r.max_depth;
        }
    }
    if (r.backtrace_frames == 0u && iteration >= 1000u) r.backtrace_frames = f1c::capture_backtrace_rvas(r.backtrace, 62u);
}

void run_one(Job& job, u64 n, RunResult& r) {
    auto bus = std::make_unique<GuestMemory>();
    auto cpu = std::make_unique<Cpu>(*bus);
    Cpu& c = *cpu;
    c.reset();
    c.attach_recompiled(job.code);
    for (u32 i = 0; i < 32u; ++i) c.gpr[i] = 0u;
    c.lr = kReturn;
    c.set_decrementer(0x7FFFFFFFu);
    if (job.ee) c.set_msr(c.msr() | (kMsrEE)); else c.set_msr(c.msr() & (~kMsrEE));
    r.base = probe_address();
    RunResult* rp = &r;
    if (job.shape == "game_803D946C") {
        const u32 obj = 0x80400000u, vtable = 0x80400100u, stack = 0x80300000u, frame = stack - 0x30u;
        c.set_entry(0x803D946Cu);
        c.gpr[1] = stack;
        c.gpr[3] = obj;
        bus->write32(obj, vtable);
        for (u32 slot : {0x18u, 0x1Cu, 0x20u}) bus->write32(vtable + slot, 0x803D8088u);
        bus->write32(frame + 0x14u, 4u);
        bus->write32(frame + 0x10u, 0u);
        c.install_hle(0x803D8088u, [](Cpu& cpu) { cpu.gpr[3] = 0u; return true; }, "f1c stub 803D8088");
        c.install_hle(0x803CBF34u, [rp, n](Cpu& cpu) {
            ++rp->iterations;
            sample(*rp, rp->iterations);
            if (rp->iterations >= n) cpu.request_stop(StopReason::HostRequest, "f1c iteration limit");
            cpu.gpr[3] = 0u;
            return true;
        }, "f1c probe 803CBF34");
    } else {
        const u32 s1 = 0x80400200u, s2 = 0x80400210u;
        c.set_entry(0x803A1AACu);
        c.gpr[1] = 0x80300000u;
        c.gpr[12] = s1;
        bus->write32(s1, 0u); bus->write32(s1 + 4u, 0xFFFFFFFFu); bus->write32(s1 + 8u, 0x803A1AD4u);
        bus->write32(s2, 0u); bus->write32(s2 + 4u, 0xFFFFFFFFu); bus->write32(s2 + 8u, 0x803A1AACu);
        c.install_hle(0x803A1AACu, [rp, n, s1](Cpu& cpu) {
            ++rp->iterations;
            sample(*rp, rp->iterations);
            if (rp->iterations > n) {
                cpu.request_stop(StopReason::HostRequest, "f1c iteration limit");
                return true;  // served: every native frame returns
            }
            cpu.gpr[12] = s1;
            return false;
        }, "f1c ptmf r12 s1");
        c.install_hle(0x803A1AD4u, [s2](Cpu& cpu) { cpu.gpr[12] = s2; return false; }, "f1c ptmf r12 s2");
    }
    r.commit_before = f1c::stack_base() - f1c::stack_limit();
    const double t0 = f1c::now_ms();
    // No budget exists: the run ends only at a stop (here the probe's HostRequest after the last iteration).
    c.run();
    r.wall_ms = f1c::now_ms() - t0;
    r.commit_after = f1c::stack_base() - f1c::stack_limit();
    r.dispatches = c.recompiled_calls;
    r.stop_reason = static_cast<int>(c.stop().reason);
    r.stop_detail = c.stop().detail;
    r.pc = c.pc;
    const u64 expected = job.shape == "game_803D946C" ? n : n + 1u;
    if (c.stop().reason != StopReason::HostRequest) r.status = "unexpected-stop";
    else if (r.iterations != expected) r.status = "wrong-iteration-count";
}

void job_thread(void* arg) {
    Job& job = *static_cast<Job*>(arg);
    job.guard_before = f1c::guard_page_below_limit();
    run_one(job, job.warm, job.warm_result);
    run_one(job, job.iterations, job.main_result);
    job.guard_after = f1c::guard_page_below_limit();
}

std::string result_json(const RunResult& r) {
    char b[768];
    std::string detail;
    for (char ch : r.stop_detail) detail += (ch == '"' || ch == '\\') ? ' ' : ch;
    // "retired" is null: native time has no instruction count (the key stays for run_f1c_stack.py).
    std::snprintf(b, sizeof(b),
                  "{\"status\":\"%s\",\"pc\":\"0x%08X\",\"r3\":%llu,\"retired\":null,\"dispatches\":%llu,\"yields\":0,"
                  "\"interpreter_exits\":0,\"hle_hits\":%llu,\"stop_reason\":%d,\"samples\":%llu,\"max_depth\":%llu,"
                  "\"commit_before\":%llu,\"commit_after\":%llu,\"wall_ms\":%.1f,\"stop_detail\":\"%s\",\"checkpoints\":[",
                  r.status.c_str(), r.pc, static_cast<unsigned long long>(r.iterations),
                  static_cast<unsigned long long>(r.dispatches), static_cast<unsigned long long>(r.iterations), r.stop_reason,
                  static_cast<unsigned long long>(r.iterations), static_cast<unsigned long long>(r.max_depth),
                  static_cast<unsigned long long>(r.commit_before), static_cast<unsigned long long>(r.commit_after), r.wall_ms,
                  detail.c_str());
    std::string j = b;
    bool first = true;
    for (std::size_t k = 0; k < kCheckpointCount; ++k) {
        if (!r.cp_seen[k]) continue;
        std::snprintf(b, sizeof(b), "%s[%llu,%llu,%llu]", first ? "" : ",", static_cast<unsigned long long>(kCheckpoints[k]),
                      static_cast<unsigned long long>(r.cp_now[k]), static_cast<unsigned long long>(r.cp_max[k]));
        j += b;
        first = false;
    }
    j += "],\"backtrace_rva\":[";
    for (std::size_t k = 0; k < r.backtrace_frames; ++k) {
        std::snprintf(b, sizeof(b), "%s\"0x%llx\"", k ? "," : "", static_cast<unsigned long long>(r.backtrace[k]));
        j += b;
    }
    return j + "]}";
}

}  // namespace

int main(int argc, char** argv) {
    std::string shape = "all", out_path;
    int ee = -1;
    u64 iterations = 1000000u, warm = 1000u, reserve_mib = 1024u;
    const char* shapes[] = {"game_803D946C", "game_803A1AAC"};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--shape") shape = value();
        else if (a == "--ee") ee = std::atoi(value().c_str());
        else if (a == "--iterations") iterations = std::strtoull(value().c_str(), nullptr, 0);
        else if (a == "--warm") warm = std::strtoull(value().c_str(), nullptr, 0);
        else if (a == "--reserve-mib") reserve_mib = std::strtoull(value().c_str(), nullptr, 0);
        else if (a == "--sampler") value();  // accepted for run_f1c_stack.py; the probe hook always samples
        else if (a == "--out") out_path = value();
        else if (a == "--list") { for (const char* s : shapes) std::printf("%s\n", s); return 0; }
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    if (out_path.empty() || (ee != 0 && ee != 1) || warm == 0u || warm >= iterations) {
        std::fprintf(stderr, "usage: f1c_game_stack_driver --shape <name|all> --ee <0|1> [--iterations N] --out file\n");
        return 2;
    }
    static RecompiledCode code;
    code.install(recomp::kFunctions, recomp::kFunctionCount);
    code.install_ranges(recomp::kRanges, recomp::kRangeCount);
    for (u32 a : {0x803D946Cu, 0x803D94ACu, 0x803D948Cu, 0x803D8088u, 0x803CBF34u, 0x803A1AACu, 0x803A1AD4u}) {
        if (code.lookup(a) == nullptr) { std::fprintf(stderr, "no native entry 0x%08X in shadow_recomp\n", a); return 3; }
    }
    std::ofstream out(out_path, std::ios::binary | std::ios::app);
    int ran = 0;
    for (const char* s : shapes) {
        if (shape != "all" && shape != s) continue;
        Job job;
        job.shape = s;
        job.ee = ee != 0;
        job.warm = warm;
        job.iterations = iterations;
        job.code = &code;
        const unsigned long exit_code = f1c::run_on_thread(static_cast<std::size_t>(reserve_mib) << 20, &job_thread, &job);
        char head[320];
        std::snprintf(head, sizeof(head),
                      "{\"shape\":\"%s\",\"control\":false,\"ee\":%d,\"sampler\":\"hook\",\"warm\":%llu,\"iterations\":%llu,"
                      "\"reserve_mib\":%llu,\"thread_exit\":\"0x%08lX\",\"guard_before\":%s,\"guard_after\":%s",
                      s, ee, static_cast<unsigned long long>(warm), static_cast<unsigned long long>(iterations),
                      static_cast<unsigned long long>(reserve_mib), exit_code, job.guard_before ? "true" : "false",
                      job.guard_after ? "true" : "false");
        out << head << ",\"warm_run\":" << result_json(job.warm_result) << ",\"main_run\":" << result_json(job.main_result)
            << "}\n";
        ++ran;
    }
    return ran ? 0 : 2;
}
