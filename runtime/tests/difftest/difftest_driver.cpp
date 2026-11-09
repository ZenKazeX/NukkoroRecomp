// Differential-harness driver (tasks F1a and F1b). Test-only; compiled from this one source:
//
//   difftest_driver_prod / difftest_f1b_driver_prod  link shadow_runtime (the PRODUCTION cpu.cpp)
//   difftest_driver_ref  / difftest_f1b_driver_ref   link shadow_runtime_reference
//                                                    (SHADOW_CPU_REFERENCE_BUILD cpu.cpp),
//                                                    DIFFTEST_REFERENCE=1
//
// Each links generated objects: real tools/ppcrecomp output for a synthetic DOL, compiled with the
// shard flags (SHADOW_ARCH_FLAGS). Guest code is only ever entered through Cpu::run (or, in the
// reference build, Cpu::run_steps / Cpu::step) with the generated table attached - the dispatcher
// production uses - never by calling a generated symbol or a ppc_ops helper directly.
//
// Native time (docs/native-time-20261005/DESIGN.md): nothing in native code counts instructions and guest
// code runs in zero game time. The time base and the decrementer are views of a game clock that only a wait
// point moves, and these vectors contain none, so TB and DEC keep their initial values in every lane. Lanes
// are compared at the boundary every one of them reaches without a count - the vector's stop address (its
// return to the sentinel, or an exception's architected boundary) - on the whole architected state, the data
// window bytes, the rest of the game's memory, the access log and the stop reason.
//
// Lanes (one JSONL record per vector per lane):
//   prod: N   native run(), fast memory path (no access trace): exactly the shipped configuration
//         NL  native run(), every guest memory access traced and logged (slow memory path)
//   ref:  R   native through the reference build's run(), fast memory path
//         RL  native, logged; then the same vector again under the differential oracle
//             (Cpu::set_verify_recompiled): every native segment is replayed by the interpreter and compared on
//             the whole architected state and every written byte, its end found without a count (it returned,
//             faulted with the same exceptions, or reached its first nested call). The oracle's VerifyReport
//             ("verify") replaces the retire_block block trace that bounded native-vs-interpreter divergences
//             by retired count; an oracle mismatch, or an oracle run that ends in a different state from the
//             logged run, fails the lane
//         I   reference interpreter Cpu::step() from the same initial state, logged, one
//             snapshot per completed instruction (project recognition policy R1 at entry, R2-R5 in step)
//         IF  interpreter, fast memory path
//         IR  reference run_steps(1) loop with NO recompiled code: R1 before every instruction, i.e.
//             an asynchronous condition is recognised at every instruction boundary (the manual's
//             policy); per-instruction snapshots                                      [F1b]
//   RETIRED 2026-10-06 (native time): NB (production run(1) loop) and N1 (reference run(1) loop with the
//   generated table). Their whole purpose was the instruction budget: run(1) yielded at every back edge, so
//   execution re-entered translated code through ranges and cold entries on every loop iteration and an
//   interrupt was recognised at every yield. No budget exists any more (Cpu::run takes none; native code
//   yields only to a stop request or a deliverable interrupt), so both would repeat N and R exactly. Asking
//   for either is an error.
//
// "retired" in a state record is the interpreter's count of completed instructions (Cpu::interpreted_
// instructions, advanced by step() exactly where it used to advance total_instructions); native lanes have
// no count and their records omit the key. Interpreter traces are indexed by it as before.
//
// Termination: LR on entry is the untranslated sentinel. A vector may add further stop addresses
// (`stops`), normally the architected exception vectors (MSR[IP]=1 puts them at 0xFFF00xxx).
// Production lanes stop with the production ExecutionFault at an untranslated stop address;
// reference native lanes stop through an HLE hook at every stop address (the hook's pc <- lr side
// effect is undone, the captured pc is reported); interpreter lanes stop stepping at a stop address.
// An exception (docs/memory-architecture-20261006): the CPU performs the architected entry (SRR0,
// SRR1, the exception MSR) and then the OS's exception entry, which these machines never install, so
// the entry is refused and the run stops (CpuStop::exception names the vector offset). The spec model
// stops at the architected vector address, so a refused entry is the run reaching that stop: the
// record names (MSR[IP] ? 0xFFF00000 : 0) | offset as its final pc and keeps the refusal in its host
// facts (`os_entry_refused`).
// A native run that never reaches a stop address - a guest loop only an event could end, such as the RAM-poll
// idle loop waiting for a decrementer edge, which can no longer fall inside guest code - spins in zero game
// time with no host callback inside it. A host watchdog ends it the way System's wait-point watchdog does
// (Cpu::request_stop_from_host; `--watchdog-ms`, default 500): status "watchdog". A logged lane stops itself
// earlier and deterministically once its access log outgrows anything the vector's step bound allows: status
// "access-limit". Interpreter lanes stay bounded by the vector's step bound ("step-limit").
//
// Host stack (native lanes; no per-block hook exists to sample it any more): the stack below the driver's
// frame is painted before each run and the deepest word the run overwrote gives the deepest host stack it
// reached. Less the lane's calibrated depth of a run that stops at once (dispatcher plus stop path), that is
// the host stack the vector's own native execution added; above `--host-stack-bound` (default 16 KiB,
// compare_f1b.py's HOST_STACK_BOUND) the lane fails with status "host-stack-bound". Only the verdict is
// written (the raw depth only when it fails), so allocator placement cannot change a record.
//
// Vector keys (tools/difftest/gen_vectors.py, gen_f1b.py) under native time:
//   residue    the time-base prescaler phase of the instruction clock: accepted, no effect (no prescaler)
//   tick N     the hardware service interval: accepted, no effect (Cpu::set_hardware_tick is deleted; the host
//              stack is measured in every native run instead)
//   irq_ext 0  the external line is asserted before entry (unchanged)
//   irq_ext K  (K > 0) RETIRED: the scripted device raised the line at the first service whose retired count
//              reached K. No instruction count exists and a line changes only at a wait point, so the vector is
//              not run; every lane reports status "irq-at-retired-count" with the initial state.
#include "gekko/cpu.h"
#include "gekko/cpu_diag.h"
#include "gekko/guest_memory.h"
#include "gekko/recomp.h"

#include <intrin.h>
#include <malloc.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace gekko;

namespace {

constexpr u32 kSentinel = 0x80009000u;
constexpr u32 kDataBase = 0x80300000u;
constexpr u32 kDataSize = 0x00008000u;
constexpr u64 kNoIrq = ~0ull;
constexpr const char* kWatchdogDetail =
    "difftest watchdog: the native run reached no stop address (a guest loop only an event can end spins in "
    "zero game time)";
constexpr const char* kAccessLimitDetail = "difftest: the guest access log outgrew the vector's step bound";
// Stack painting window below the measuring frame, and its fill word.
constexpr std::size_t kStackPaintBytes = 128u * 1024u;
constexpr u64 kStackPaint = 0xD1FF7E57C0DE5EEDull;

#if defined(DIFFTEST_REFERENCE)
constexpr bool kReference = true;
#else
constexpr bool kReference = false;
#endif

struct MemBlob {
    u32 address = 0;
    std::vector<u8> bytes;
};

struct Vector {
    std::string id;
    u32 entry = 0;
    u32 gpr[32]{};
    u64 fpr[32][2]{};
    u32 cr = 0, xer = 0, lr = 0, ctr = 0, msr = 0, fpscr = 0, dec = 0;
    u64 tb = 0;
    u32 residue = 0;       // accepted, no effect under native time (see the header)
    std::vector<std::pair<u32, u32>> spr;
    std::vector<std::pair<u32, u32>> sr;
    bool reserve = false;
    u32 reserve_addr = 0;
    std::vector<MemBlob> mem;
    u64 max_steps = 4096;
    u64 tick = 0;          // accepted, no effect under native time (see the header)
    u64 irq_ext = kNoIrq;  // 0: the external line asserted before entry; > 0: retired (see the header)
    std::set<u32> stops;   // termination addresses in addition to the sentinel
};

struct CodeRow {
    u32 address = 0;
    std::vector<u32> words;
};

[[noreturn]] void die(const std::string& why) {
    std::fprintf(stderr, "difftest_driver: %s\n", why.c_str());
    std::exit(2);
}

std::vector<u8> hex_bytes(const std::string& text) {
    if (text.size() % 2 != 0) die("odd hex byte string");
    std::vector<u8> out(text.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<u8>(std::strtoul(text.substr(2 * i, 2).c_str(), nullptr, 16));
    return out;
}

void load_vectors(const std::string& path, std::vector<CodeRow>& rows, std::vector<Vector>& vectors) {
    std::ifstream in(path);
    if (!in) die("cannot read " + path);
    std::string line;
    Vector* current = nullptr;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream s(line);
        std::string key;
        s >> key;
        auto hex = [&](u64& v) { std::string t; s >> t; v = std::strtoull(t.c_str(), nullptr, 16); };
        auto hex32 = [&]() { u64 v = 0; hex(v); return static_cast<u32>(v); };
        if (key == "code") {
            CodeRow row;
            row.address = hex32();
            std::string t;
            while (s >> t) row.words.push_back(static_cast<u32>(std::strtoul(t.c_str(), nullptr, 16)));
            rows.push_back(std::move(row));
        } else if (key == "vector") {
            vectors.emplace_back();
            current = &vectors.back();
            s >> current->id;
        } else if (current == nullptr) {
            die("field before vector: " + line);
        } else if (key == "entry") current->entry = hex32();
        else if (key == "gpr") for (u32& g : current->gpr) g = hex32();
        else if (key == "fpr") for (auto& f : current->fpr) { hex(f[0]); hex(f[1]); }
        else if (key == "cr") current->cr = hex32();
        else if (key == "xer") current->xer = hex32();
        else if (key == "lr") current->lr = hex32();
        else if (key == "ctr") current->ctr = hex32();
        else if (key == "msr") current->msr = hex32();
        else if (key == "fpscr") current->fpscr = hex32();
        else if (key == "dec") current->dec = hex32();
        else if (key == "tb") hex(current->tb);
        else if (key == "residue") s >> current->residue;
        else if (key == "spr") { u32 n = 0; s >> n; current->spr.emplace_back(n, hex32()); }
        else if (key == "sr") { u32 n = 0; s >> n; if (n >= 16) die("sr index"); current->sr.emplace_back(n, hex32()); }
        else if (key == "reserve") { int r = 0; s >> r; current->reserve = r != 0; current->reserve_addr = hex32(); }
        else if (key == "mem") { MemBlob b; b.address = hex32(); std::string t; s >> t; b.bytes = hex_bytes(t); current->mem.push_back(std::move(b)); }
        else if (key == "max") s >> current->max_steps;
        else if (key == "tick") s >> current->tick;
        else if (key == "irq_ext") s >> current->irq_ext;
        else if (key == "stops") { std::string t; while (s >> t) current->stops.insert(static_cast<u32>(std::strtoul(t.c_str(), nullptr, 16))); }
        else if (key == "end") current = nullptr;
        else die("unknown key: " + key);
    }
}

// ---- JSON helpers -------------------------------------------------------------------
std::string h32(u32 v) { char b[16]; std::snprintf(b, sizeof(b), "\"0x%08X\"", v); return b; }
std::string h64(u64 v) { char b[32]; std::snprintf(b, sizeof(b), "\"0x%016" PRIX64 "\"", v); return b; }
std::string hexstr(const u8* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (std::size_t i = 0; i < n; ++i) { s[2 * i] = d[p[i] >> 4]; s[2 * i + 1] = d[p[i] & 15]; }
    return s;
}
std::string quote(const std::string& text) {
    std::string out = "\"";
    for (char ch : text) {
        if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
        else if (static_cast<unsigned char>(ch) < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", ch); out += b; }
        else out += ch;
    }
    return out + "\"";
}

// `retired`: write the interpreter's completed-instruction count (interpreter lanes). Native lanes have no
// instruction count and omit the key.
std::string state_json(const Cpu& c, u32 pc, bool retired) {
    std::string j = "{\"gpr\":[";
    for (int i = 0; i < 32; ++i) { if (i) j += ','; j += h32(c.gpr[i]); }
    j += "],\"fpr\":[";
    for (int i = 0; i < 32; ++i) { if (i) j += ','; j += "[" + h64(c.fpr[i].bits[0]) + "," + h64(c.fpr[i].bits[1]) + "]"; }
    j += "],\"cr\":" + h32(c.cr) + ",\"xer\":" + h32(c.xer) + ",\"lr\":" + h32(c.lr) + ",\"ctr\":" + h32(c.ctr);
    j += ",\"msr\":" + h32(c.msr()) + ",\"fpscr\":" + h32(c.fpscr);
    j += ",\"pc\":" + h32(pc);
    j += ",\"spr\":[";
    bool first = true;
    for (u32 i = 0; i < 1024u; ++i) {
        // The decrementer is Cpu::decrementer() (a view of the game clock), not its spr[] slot.
        const u32 value = i == SPR_DEC ? c.decrementer() : c.spr[i];
        if (value == 0u) continue;
        if (!first) j += ',';
        first = false;
        j += "[" + std::to_string(i) + "," + h32(value) + "]";
    }
    j += "],\"sr\":[";
    for (int i = 0; i < 16; ++i) { if (i) j += ','; j += h32(c.sr[i]); }
    j += "],\"tb\":" + h64(c.time_base());
    if (retired) j += ",\"retired\":" + std::to_string(c.interpreted_instructions());
    j += ",\"reserve\":" + std::string(c.reservation ? "true" : "false") + ",\"reserve_addr\":" + h32(c.reservation_address);
    j += ",\"line_ext\":" + std::string(c.external_interrupt() ? "true" : "false");
    j += ",\"line_dec\":" + std::string(c.decrementer_pending() ? "true" : "false");
    const auto& f = c.memory_fault();
    j += ",\"fault\":{\"latched\":" + std::string(f.latched ? "true" : "false") + ",\"write\":" +
         std::string(f.write ? "true" : "false") + ",\"effective\":" + h32(f.effective) + ",\"pc\":" + h32(f.pc) + "}";
    return j + "}";
}

// Runs of bytes in the data window that differ from the vector's initial window image.
std::string window_diff(const GuestMemory& memory, const std::vector<u8>& initial) {
    const u8* now = memory.peek_pointer(kDataBase, kDataSize);
    std::string j = "[";
    bool first = true;
    for (u32 i = 0; i < kDataSize;) {
        if (now[i] == initial[i]) { ++i; continue; }
        u32 k = i;
        while (k < kDataSize && now[k] != initial[k]) ++k;
        if (!first) j += ',';
        first = false;
        j += "[" + h32(kDataBase + i) + ",\"" + hexstr(now + i, k - i) + "\"]";
        i = k;
    }
    return j + "]";
}

// ---- native-time host machinery -----------------------------------------------------
//
// A guest loop that only an event can end spins forever in zero game time, and an unlogged native run makes
// no host callback inside it. The cross-thread stop System's wait-point watchdog uses
// (Cpu::request_stop_from_host) is the one way to end it; one worker thread serves every run of the process.
class Watchdog {
public:
    explicit Watchdog(unsigned milliseconds) : milliseconds_(milliseconds), worker_([this] { loop(); }) {}
    ~Watchdog() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        worker_.join();
    }
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

    // Armed on the guest thread right before Cpu::run, for that run only.
    void arm(Cpu& cpu) {
        std::lock_guard<std::mutex> lock(mutex_);
        cpu_ = &cpu;
        fired_ = false;
        deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds_);
        ++generation_;
        wake_.notify_all();
    }
    // After Cpu::run returned. True when the stop was posted for this run; the post is complete (it happens
    // under the same lock), but it may have landed after the run ended and then still awaits a dispatcher.
    bool disarm() {
        std::lock_guard<std::mutex> lock(mutex_);
        cpu_ = nullptr;
        ++generation_;
        wake_.notify_all();
        return fired_;
    }

private:
    void loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!quit_) {
            if (cpu_ == nullptr) {
                wake_.wait(lock);
                continue;
            }
            const u64 generation = generation_;
            if (wake_.wait_until(lock, deadline_, [&] { return quit_ || generation_ != generation; })) continue;
            fired_ = true;
            cpu_->request_stop_from_host(StopReason::HostRequest, kWatchdogDetail);
            cpu_ = nullptr;
        }
    }

    const unsigned milliseconds_;
    std::mutex mutex_;
    std::condition_variable wake_;
    Cpu* cpu_ = nullptr;
    bool fired_ = false;
    bool quit_ = false;
    u64 generation_ = 0;
    std::chrono::steady_clock::time_point deadline_{};
    std::thread worker_;  // declared last: it starts after every member above is initialised
};

GEKKO_NOINLINE std::uintptr_t probe_address() {
    return reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
}

// Fills kStackPaintBytes of stack below the caller's frame (_alloca probes page by page, so the guard page
// below the committed stack is touched in order) and returns the lowest painted address.
GEKKO_NOINLINE std::uintptr_t paint_stack() {
    auto* region = static_cast<volatile u64*>(_alloca(kStackPaintBytes));
    for (std::size_t i = 0; i < kStackPaintBytes / sizeof(u64); ++i) region[i] = kStackPaint;
    return reinterpret_cast<std::uintptr_t>(region);
}

// The lowest painted word something overwrote since paint_stack (the region top if nothing did).
GEKKO_NOINLINE std::uintptr_t deepest_overwrite(std::uintptr_t bottom) {
    const auto* region = reinterpret_cast<const volatile u64*>(bottom);
    std::size_t i = 0;
    while (i < kStackPaintBytes / sizeof(u64) && region[i] == kStackPaint) ++i;
    return bottom + i * sizeof(u64);
}

// One Cpu::run under the watchdog with the stack painted below this frame: returns the host stack depth the
// run reached (bytes between this frame and the deepest painted word it overwrote); `fired` as
// Watchdog::disarm.
GEKKO_NOINLINE u64 run_measured(Cpu& c, Watchdog& watchdog, bool& fired) {
    const std::uintptr_t top = probe_address();
    const std::uintptr_t bottom = paint_stack();
    watchdog.arm(c);
    c.run();
    fired = watchdog.disarm();
    const std::uintptr_t deepest = deepest_overwrite(bottom);
    return top > deepest ? static_cast<u64>(top - deepest) : 0u;
}

// An exception whose OS entry was refused (none is installed in these machines): the architected vector
// address the spec model stops at, or 0 for any other stop.
u32 exception_stop_address(const Cpu& c) {
    const CpuStop& stop = c.stop();
    if (stop.reason != StopReason::ExecutionFault || stop.exception == 0u) return 0u;
    return ((c.msr() & kMsrIP) != 0u ? 0xFFF00000u : 0u) | stop.exception;
}

bool watchdog_stop(const Cpu& c) {
    return c.stop().reason == StopReason::HostRequest && c.stop().detail.rfind(kWatchdogDetail, 0) == 0;
}

struct Access {
    bool write;
    u32 address;
    u32 size;
    std::string bytes;  // materialised value (reads: before the access; writes: after it)
};

enum class Engine { Native, Interpreter, InterpreterDispatch };

class Lane {
public:
    Lane(std::string name, Engine engine, bool logged, bool trace, bool oracle, RecompiledCode* code,
         const std::vector<CodeRow>& rows, const std::set<u32>& all_stops, Watchdog& watchdog, u64 host_stack_bound)
        : name_(std::move(name)), engine_(engine), logged_(logged), trace_(trace), oracle_(oracle), code_(code),
          watchdog_(watchdog), host_stack_bound_(host_stack_bound) {
        memory_ = std::make_unique<GuestMemory>();
        cpu_ = std::make_unique<Cpu>(*memory_);
        for (const CodeRow& row : rows) {
            for (std::size_t i = 0; i < row.words.size(); ++i) {
                memory_->write32(row.address + static_cast<u32>(4u * i), row.words[i]);
            }
            code_ranges_.emplace_back(row.address, row.address + static_cast<u32>(4u * row.words.size()));
        }
        // Every mapped region's committed bytes, in region order.
        for (const auto& region : memory_->regions()) {
            const auto bytes = memory_->window_bytes(region.base, region.base + region.committed);
            pristine_.insert(pristine_.end(), bytes.begin(), bytes.end());
        }
        cpu_->reset();
        if (kReference) {
            // Every address any vector may stop at. The hook serves (stops) only when the address
            // is a stop of the vector being run; otherwise it declines and execution continues.
            std::set<u32> hooks(all_stops);
            hooks.insert(kSentinel);
            for (u32 address : hooks) {
                cpu_->install_hle(address, [this, address](Cpu& c) {
                    if (address != kSentinel && current_stops_->count(address) == 0) return false;
                    hle_hit_ = true;
                    hle_pc_ = c.pc;
                    c.request_stop(StopReason::HleExit, "difftest stop");
                    return true;
                }, "difftest stop");
            }
        }
        if (native()) calibrate_host_stack();
    }

    bool native() const { return engine_ == Engine::Native; }

    std::string run(const Vector& v) {
        Cpu& c = *cpu_;
        GuestMemory& memory = *memory_;
        if (v.tick > 1u && v.irq_ext != kNoIrq) die(v.id + ": irq_ext needs tick 1");
        current_stops_ = &v.stops;
        const std::vector<u8> initial = prepare(v);
        const bool retired = !native();
        const std::string initial_state = state_json(c, v.entry, retired);

        log_.clear();
        pending_write_ = false;
        log_overflow_ = false;
        trace_json_.clear();
        trace_count_ = 0;
        hle_hit_ = false;
        hle_pc_ = 0;
        std::string status = "ok";
        std::string detail_override;
        bool ran = true;
        u64 steps = 0;
        u64 dispatches = 0;
        bool stack_measured = false;
        u64 stack_spread = 0;
        const u64 non_native_before = g_non_native_ppc_executions.load();
        const auto at_stop = [&](u32 pc) { return pc == kSentinel || v.stops.count(pc) != 0; };
        if (v.irq_ext != kNoIrq && v.irq_ext != 0u) {
            // The scripted device raised the line at a retired count; no instruction count exists and a line
            // changes only at a wait point, so the vector has no native meaning and is not run in any lane.
            ran = false;
            status = "irq-at-retired-count";
            detail_override = "irq_ext " + std::to_string(v.irq_ext) +
                              ": the line rose at a retired-instruction count; native time has no instruction "
                              "count and lines change only at wait points (vector not run)";
        }
        if (ran && logged_) {
            // A logged lane makes at most `access_limit_` guest accesses: the vector's step bound times the most
            // any one instruction makes, with headroom. Past it the run stops itself (deterministically, unlike
            // the watchdog) and the log is dropped.
            access_limit_ = 128u * v.max_steps + 4096u;
            memory.set_access_trace([this](u32 address, std::size_t size, bool writing) {
                if (log_overflow_) return;
                materialise();
                if (log_.size() >= access_limit_) {
                    log_overflow_ = true;
                    log_.clear();
                    pending_write_ = false;
                    cpu_->request_stop(StopReason::HostRequest, kAccessLimitDetail);
                    return;
                }
                Access a{writing, address, static_cast<u32>(size), {}};
                if (writing) {
                    pending_write_ = true;
                } else {
                    a.bytes = bytes_at(address, size);
                }
                log_.push_back(std::move(a));
            });
        }
        c.attach_recompiled(native() ? code_ : nullptr);

        if (!ran) {
            // Nothing runs.
        } else if (native()) {
            bool fired = false;
            const u64 depth = run_measured(c, watchdog_, fired);
            ++dispatches;
            note_watchdog(c, fired);
            stack_measured = true;
            stack_spread = depth > stack_baseline_ ? depth - stack_baseline_ : 0u;
            if (!c.stopped()) status = "run-returned-without-stop";
        } else {
#if defined(DIFFTEST_REFERENCE)
            if (engine_ == Engine::Interpreter) {
                // Cpu::step exists only in the reference build (production forbids interpretation).
                // Recognition point R1 is Cpu::run's, taken before native code is entered; this lane
                // has no run(), so R1 at entry is applied here with the public entry sequence
                // take_pending_interrupt uses for the external line (raise, pc <- npc). A decrementer
                // condition is never latched at entry by the generator.
                if (c.external_interrupt() && (c.msr() & kMsrEE) != 0u) {
                    c.raise_exception(Exception::ExternalInterrupt);
                    c.pc = c.npc;
                }
            }
            // One snapshot per COMPLETED instruction (Cpu::interpreted_instructions), so trace index k is the
            // state after k + 1 instructions completed (a fault that completes nothing, or a served stop hook,
            // adds none).
            u64 calls = 0;
            // At a stop address a deliverable condition is still taken first: native lanes return to
            // Cpu::run there, whose R1 check takes it before the stop is reached.
            while (!c.stopped() && !(at_stop(c.pc) && !c.interrupt_pending())) {
                if (calls >= v.max_steps) { status = "step-limit"; break; }
                const u32 pre = c.pc;
                const bool pend = c.interrupt_pending();
                const u64 before = c.interpreted_instructions();
                if (engine_ == Engine::Interpreter) {
                    c.step();
                } else {
                    // IR: one dispatch, R1 before every instruction (no recompiled code attached).
                    c.run_steps(1u);
                    ++dispatches;
                }
                ++calls;
                if (c.interpreted_instructions() != before) {
                    ++steps;
                    if (trace_ && !hle_hit_) snapshot(initial, pre, pend);
                } else if (!c.stopped() && c.pc == pre) {
                    status = "no-progress";
                    break;
                }
            }
            if (c.stopped() && !hle_hit_) status = "stopped";
#else
            die("interpreter lane in the production build");
#endif
        }
        const u64 non_native = g_non_native_ppc_executions.load() - non_native_before;
        if (logged_) {
            materialise();
            memory.set_access_trace({});
        }

        u32 final_pc = ran ? c.pc : v.entry;
        const CpuStop stop = c.stop();
        const u32 exception_stop = ran ? exception_stop_address(c) : 0u;
        if (ran && exception_stop != 0u && at_stop(exception_stop)) {
            final_pc = exception_stop;           // the architected vector the refused OS entry stands for
            if (status == "stopped") status = "ok";
        } else if (ran && native()) {
            if (kReference) {
                if (stop.reason == StopReason::HleExit && hle_hit_) {
                    final_pc = hle_pc_;          // undo the served hook's pc <- lr
                } else if (status == "ok") {
                    status = "native-stop-not-at-stop-address";
                }
            } else {
                const std::string expected = "native execution cannot continue at 0x";
                const bool fault_stop = stop.reason == StopReason::ExecutionFault &&
                                        stop.detail.compare(0, expected.size(), expected) == 0;
                if (!(fault_stop && at_stop(c.pc)) && status == "ok") {
                    status = "native-stop-not-at-stop-address";
                }
            }
        } else if (hle_hit_) {
            final_pc = hle_pc_;
        }
        // Which bound ended a run that reached no stop address.
        if (log_overflow_) status = "access-limit";
        else if (ran && native() && watchdog_stop(c)) status = "watchdog";
        const bool stack_ok = !stack_measured || stack_spread <= host_stack_bound_;
        if (!stack_ok && status == "ok") status = "host-stack-bound";

        const std::string final_state = state_json(c, final_pc, retired);
        const std::string mem = window_diff(memory, initial);
        // Unlogged lanes: every mapped byte outside the data window must be identical to the pristine
        // image (a wild store would otherwise be invisible). Logged lanes see every guest access in
        // their log instead. The locked cache is also checked once per lane (finish()).
        bool clean = true;
        if (!logged_) {
            clean = outside_clean(memory);
            if (!clean) restore_outside(memory);
        }
        std::string host = "{\"non_native\":" + std::to_string(non_native) +
                           ",\"interpreted\":" + std::to_string(c.interpreted_instructions()) +
                           ",\"native_calls\":" + std::to_string(c.recompiled_calls) +
                           ",\"interpreter_exits\":" + std::to_string(c.interpreter_exits) +
                           ",\"steps\":" + std::to_string(steps) + ",\"dispatches\":" + std::to_string(dispatches) +
                           ",\"stop_reason\":" + std::to_string(static_cast<int>(stop.reason)) +
                           ",\"stop_detail\":" + quote(detail_override.empty() ? stop.detail : detail_override) +
                           ",\"hle_stop\":" + std::string(hle_hit_ ? "true" : "false") +
                           ",\"os_entry_refused\":" + std::to_string(exception_stop != 0u ? stop.exception : 0u) +
                           ",\"yield\":" + std::to_string(c.fallback_total(FallbackReason::Yield)) +
                           ",\"not_recompiled\":" + std::to_string(c.fallback_total(FallbackReason::NotRecompiled)) +
                           ",\"mid_entries\":" + std::to_string(c.dispatch_entered_mid_function) +
                           ",\"host_stack_within_bound\":" +
                           std::string(!stack_measured ? "null" : stack_ok ? "true" : "false");
        if (!stack_ok) host += ",\"host_stack_spread\":" + std::to_string(stack_spread);
        host += "}";
        std::string log;
        if (logged_) {
            log = ",\"log\":[";
            bool first = true;
            for (const Access& a : log_) {
                if (is_code(a.address) || (a.address & ~3u) == kSentinel) continue;
                if (!first) log += ',';
                first = false;
                log += "[\"" + std::string(a.write ? "w" : "r") + "\"," + h32(a.address) + "," + std::to_string(a.size) +
                       ",\"" + a.bytes + "\"]";
            }
            log += "]";
            if (log_overflow_) log += ",\"log_truncated\":true";
        }

        std::string verify;
#if defined(DIFFTEST_REFERENCE)
        if (oracle_ && ran) {
            std::string oracle_status;
            verify = ",\"verify\":" + run_oracle(v, final_state, mem, oracle_status);
            if (status == "ok") {
                if (c.verify_report().mismatches != 0u) status = "oracle-mismatch";
                else if (oracle_status != "ok") status = "oracle-" + oracle_status;
            }
        }
#endif

        std::string j = "{\"id\":" + quote(v.id) + ",\"lane\":" + quote(name_) + ",\"status\":" + quote(status);
        j += ",\"build\":\"" + std::string(kReference ? "reference" : "production") + "\"";
        j += ",\"initial\":" + initial_state;
        j += ",\"final\":" + final_state;
        j += ",\"mem\":" + mem;
        j += ",\"mem_clean\":" + std::string(clean ? "true" : "false");
        j += ",\"host\":" + host;
        j += log;
        if (trace_) j += ",\"trace\":[" + trace_json_ + "]";
        j += verify;
        j += "}";
        return j;
    }

    // Lane-level check: no vector reached the locked-cache scratch.
    std::string finish() const {
        bool cache = true;
        std::size_t offset = 0;
        for (const auto& region : memory_->regions()) {
            if (region.kind == GuestMemory::RegionKind::LockedCache) {
                const u8* now = memory_->peek_pointer(region.base, region.committed);
                cache = cache && now != nullptr && std::memcmp(now, pristine_.data() + offset, region.committed) == 0;
            }
            offset += region.committed;
        }
        return "{\"lane_summary\":" + quote(name_) + ",\"locked_cache_clean\":" + (cache ? "true" : "false") + "}";
    }

private:
    // Fresh machine state for one vector (reset keeps the Bus, the hooks and the attached code).
    std::vector<u8> prepare(const Vector& v) {
        Cpu& c = *cpu_;
        GuestMemory& memory = *memory_;
        c.reset();
        c.clear_stop();
        if (drain_owed_) drain_host_stop();
        memory.clear_block(kDataBase, kDataSize);
        for (const MemBlob& blob : v.mem) {
            if (blob.address < kDataBase || blob.address + blob.bytes.size() > kDataBase + kDataSize)
                die(v.id + ": memory blob outside the data window");
            memory.write_block(blob.address, blob.bytes.data(), blob.bytes.size());
        }
        std::vector<u8> initial = memory.window_bytes(kDataBase, kDataBase + kDataSize);
        // `residue` (the instruction clock's prescaler phase) has no native meaning: the time base and the
        // decrementer are views of the game clock, which nothing moves while these vectors run.
        for (int i = 0; i < 32; ++i) {
            c.gpr[i] = v.gpr[i];
            c.fpr[i].bits[0] = v.fpr[i][0];
            c.fpr[i].bits[1] = v.fpr[i][1];
        }
        c.cr = v.cr; c.xer = v.xer; c.lr = v.lr; c.ctr = v.ctr; c.set_msr(v.msr); c.fpscr = v.fpscr;
        c.set_time_base(v.tb);
        c.set_decrementer(v.dec);
        for (const auto& [number, value] : v.spr) {
            if (number == SPR_DEC) c.set_decrementer(value);
            else c.spr[number] = value;
        }
        for (const auto& [number, value] : v.sr) c.sr[number] = value;
        c.reservation = v.reserve;
        c.reservation_address = v.reserve_addr;
        c.select_memory_window();
        c.set_entry(v.entry);
        if (v.irq_ext == 0u) c.set_external_interrupt(true);
        return initial;
    }

    // A watchdog stop posted after its run had already ended still waits for a dispatcher: serve it now, on a
    // freshly reset machine (no latched fault can keep run() from reaching its first dispatch), before any guest
    // state is set. The dispatcher serves a posted host stop before it dispatches anything; pc is the untranslated
    // sentinel all the same, so even without a pending stop this run stops at once without executing guest code.
    void drain_host_stop() {
        Cpu& c = *cpu_;
        static const std::set<u32> no_stops;
        const std::set<u32>* const stops = current_stops_;
        current_stops_ = &no_stops;
        c.lr = kSentinel;
        c.set_entry(kSentinel);
        c.run();
        c.clear_stop();
        current_stops_ = stops;
        hle_hit_ = false;
        hle_pc_ = 0;
        drain_owed_ = false;
    }

    void note_watchdog(const Cpu& c, bool fired) {
        if (fired && !watchdog_stop(c)) drain_owed_ = true;
    }

    // The depth of a native run that stops at its first dispatch (the dispatcher and the stop path the
    // vectors end with: production's untranslated-address stop, the reference stop hook), the smallest of
    // three, so first-use paths do not inflate it.
    void calibrate_host_stack() {
        Cpu& c = *cpu_;
        static const std::set<u32> no_stops;
        current_stops_ = &no_stops;
        c.attach_recompiled(code_);
        u64 baseline = ~0ull;
        for (int i = 0; i < 3; ++i) {
            c.reset();
            c.clear_stop();
            if (drain_owed_) drain_host_stop();
            c.lr = kSentinel;
            c.set_entry(kSentinel);
            bool fired = false;
            baseline = std::min(baseline, run_measured(c, watchdog_, fired));
            note_watchdog(c, fired);
            if (!c.stopped() || watchdog_stop(c)) die(name_ + ": the host stack calibration run did not stop at once");
        }
        hle_hit_ = false;
        hle_pc_ = 0;
        stack_baseline_ = baseline;
        if (stack_baseline_ + host_stack_bound_ + 4096u > kStackPaintBytes)
            die(name_ + ": the stack paint window is too small for the calibrated depth plus the bound");
    }

#if defined(DIFFTEST_REFERENCE)
    // RL: the vector again under the differential oracle, unlogged. Returns the "verify" object; `status`
    // receives the oracle run's own end ("ok", "watchdog", "native-stop-not-at-stop-address" or
    // "final-differs": its final state, data window or outside-window bytes differ from the logged run's).
    std::string run_oracle(const Vector& v, const std::string& logged_final, const std::string& logged_mem,
                           std::string& status) {
        Cpu& c = *cpu_;
        GuestMemory& memory = *memory_;
        const std::vector<u8> initial = prepare(v);
        hle_hit_ = false;
        hle_pc_ = 0;
        c.attach_recompiled(code_);
        c.set_verify_recompiled(true, 8u);
        watchdog_.arm(c);
        c.run();
        const bool fired = watchdog_.disarm();
        note_watchdog(c, fired);
        c.set_verify_recompiled(false);
        status = "ok";
        u32 final_pc = c.pc;
        const u32 exception_stop = exception_stop_address(c);
        const std::set<u32>& stops = *current_stops_;
        if (watchdog_stop(c)) {
            status = "watchdog";
        } else if (exception_stop != 0u && (exception_stop == kSentinel || stops.count(exception_stop) != 0u)) {
            final_pc = exception_stop;
        } else if (c.stop().reason == StopReason::HleExit && hle_hit_) {
            final_pc = hle_pc_;
        } else {
            status = "native-stop-not-at-stop-address";
        }
        const bool clean = outside_clean(memory);
        if (!clean) restore_outside(memory);
        const bool same = state_json(c, final_pc, false) == logged_final && window_diff(memory, initial) == logged_mem;
        if (status == "ok" && (!same || !clean)) status = "final-differs";
        const Cpu::VerifyReport& r = c.verify_report();
        std::string j = "{\"status\":" + quote(status) + ",\"verified\":" + std::to_string(r.verified) +
                        ",\"skipped_device\":" + std::to_string(r.skipped_device) +
                        ",\"skipped_exit\":" + std::to_string(r.skipped_exit) +
                        ",\"skipped_hle\":" + std::to_string(r.skipped_hle) +
                        ",\"mismatches\":" + std::to_string(r.mismatches) +
                        ",\"verified_instructions\":" + std::to_string(r.verified_instructions) +
                        ",\"final_equal\":" + std::string(same ? "true" : "false") +
                        ",\"mem_clean\":" + std::string(clean ? "true" : "false") + ",\"messages\":[";
        for (std::size_t i = 0; i < r.messages.size(); ++i) j += (i ? "," : "") + quote(r.messages[i]);
        j += "],\"mismatch_messages\":[";
        for (std::size_t i = 0; i < r.mismatch_messages.size(); ++i) j += (i ? "," : "") + quote(r.mismatch_messages[i]);
        return j + "]}";
    }
#endif

    void materialise() {
        if (!pending_write_) return;
        Access& a = log_.back();
        a.bytes = bytes_at(a.address, a.size);
        pending_write_ = false;
    }
    // The bytes at a guest address as hex (unmapped bytes read as zero; the peek is never traced).
    std::string bytes_at(u32 address, std::size_t size) const {
        std::vector<u8> bytes(size);
        memory_->peek_block(address, bytes.data(), size);
        return hexstr(bytes.data(), size);
    }
    bool is_code(u32 address) const {
        for (const auto& [lo, hi] : code_ranges_) if (address >= lo && address < hi) return true;
        return false;
    }
    void snapshot(const std::vector<u8>& initial, u32 pre, bool pend) {
        if (trace_count_++ > 20000u) return;
        if (!trace_json_.empty()) trace_json_ += ',';
        trace_json_ += "{\"s\":" + state_json(*cpu_, cpu_->pc, !native()) + ",\"mem\":" + window_diff(*memory_, initial);
        if (!native()) trace_json_ += ",\"pre\":" + h32(pre) + ",\"pend\":" + std::string(pend ? "true" : "false");
        trace_json_ += "}";
    }
    // Every committed span of every region, outside the data window: [address, end, pristine offset).
    template <typename Visit>
    void outside_spans(const GuestMemory& memory, Visit visit) const {
        std::size_t offset = 0;
        for (const auto& region : memory.regions()) {
            const u32 begin = region.base, end = region.base + region.committed;
            if (kDataBase >= begin && kDataBase + kDataSize <= end) {
                visit(begin, kDataBase, offset);
                visit(kDataBase + kDataSize, end, offset + (kDataBase + kDataSize - begin));
            } else {
                visit(begin, end, offset);
            }
            offset += region.committed;
        }
    }
    bool outside_clean(const GuestMemory& memory) const {
        bool clean = true;
        outside_spans(memory, [&](u32 begin, u32 end, std::size_t offset) {
            if (end <= begin || !clean) return;
            const u8* now = memory.peek_pointer(begin, end - begin);
            clean = now != nullptr && std::memcmp(now, pristine_.data() + offset, end - begin) == 0;
        });
        return clean;
    }
    void restore_outside(GuestMemory& memory) {
        outside_spans(memory, [&](u32 begin, u32 end, std::size_t offset) {
            if (end > begin) memory.write_block(begin, pristine_.data() + offset, end - begin);
        });
    }

    std::string name_;
    Engine engine_;
    bool logged_, trace_, oracle_;
    RecompiledCode* code_;
    Watchdog& watchdog_;
    u64 host_stack_bound_;
    u64 stack_baseline_ = 0;
    std::unique_ptr<GuestMemory> memory_;
    std::unique_ptr<Cpu> cpu_;
    std::vector<std::pair<u32, u32>> code_ranges_;
    std::vector<u8> pristine_;  // every region's committed bytes at lane creation, in region order
    std::vector<Access> log_;
    bool pending_write_ = false;
    bool log_overflow_ = false;
    u64 access_limit_ = 0;
    std::string trace_json_;
    u64 trace_count_ = 0;
    bool hle_hit_ = false;
    u32 hle_pc_ = 0;
    bool drain_owed_ = false;
    const std::set<u32>* current_stops_ = nullptr;
};

}  // namespace

int main(int argc, char** argv) {
    std::string vectors_path, out_path, lanes_arg, only_arg;
    unsigned watchdog_ms = 500u;
    u64 host_stack_bound = 16u * 1024u;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string { if (i + 1 >= argc) die("missing value for " + a); return argv[++i]; };
        if (a == "--vectors") vectors_path = value();
        else if (a == "--out") out_path = value();
        else if (a == "--lanes") lanes_arg = value();
        else if (a == "--only") only_arg = value();
        else if (a == "--watchdog-ms") watchdog_ms = static_cast<unsigned>(std::strtoul(value().c_str(), nullptr, 0));
        else if (a == "--host-stack-bound") host_stack_bound = std::strtoull(value().c_str(), nullptr, 0);
        else die("unknown argument " + a);
    }
    if (vectors_path.empty() || out_path.empty() || watchdog_ms == 0u) {
        std::fprintf(stderr, "usage: difftest_driver --vectors vectors.txt --out results.jsonl [--lanes L,..] [--only id,..] "
                             "[--watchdog-ms N] [--host-stack-bound BYTES]\n");
        return 2;
    }
    if (lanes_arg.empty()) lanes_arg = kReference ? "R,RL,I,IF" : "N,NL";
    std::set<std::string> only;
    {
        std::stringstream s(only_arg);
        std::string t;
        while (std::getline(s, t, ',')) if (!t.empty()) only.insert(t);
    }

    std::vector<CodeRow> rows;
    std::vector<Vector> vectors;
    load_vectors(vectors_path, rows, vectors);
    std::set<u32> all_stops;
    for (const Vector& v : vectors) all_stops.insert(v.stops.begin(), v.stops.end());

    static RecompiledCode code;
    code.install(recomp::kFunctions, recomp::kFunctionCount);
    code.install_ranges(recomp::kRanges, recomp::kRangeCount);

    Watchdog watchdog(watchdog_ms);
    std::vector<std::unique_ptr<Lane>> lanes;
    std::stringstream ls(lanes_arg);
    std::string lane;
    const auto add = [&](const char* name, Engine engine, bool logged, bool trace, bool oracle) {
        lanes.push_back(std::make_unique<Lane>(name, engine, logged, trace, oracle, &code, rows, all_stops, watchdog,
                                               host_stack_bound));
    };
    while (std::getline(ls, lane, ',')) {
        if (lane == "NB" || lane == "N1")
            die("lane " + lane + " is retired (native time): it existed for the instruction budget's run(1) yields at "
                "every back edge, and no budget exists any more - it would repeat " + (lane == "NB" ? "N" : "R") +
                " exactly (see the header of difftest_driver.cpp)");
        if (lane == "N" && !kReference) add("N", Engine::Native, false, false, false);
        else if (lane == "NL" && !kReference) add("NL", Engine::Native, true, false, false);
        else if (lane == "R" && kReference) add("R", Engine::Native, false, false, false);
        else if (lane == "RL" && kReference) add("RL", Engine::Native, true, false, true);
        else if (lane == "I" && kReference) add("I", Engine::Interpreter, true, true, false);
        else if (lane == "IF" && kReference) add("IF", Engine::Interpreter, false, false, false);
        else if (lane == "IR" && kReference) add("IR", Engine::InterpreterDispatch, false, true, false);
        else die("lane " + lane + " is not available in the " + (kReference ? "reference" : "production") + " build");
    }

    std::ofstream out(out_path, std::ios::binary);
    if (!out) die("cannot write " + out_path);
    u64 records = 0;
    for (const Vector& v : vectors) {
        if (!only.empty() && only.count(v.id) == 0) continue;
        for (auto& l : lanes) {
            out << l->run(v) << '\n';
            ++records;
        }
    }
    for (auto& l : lanes) out << l->finish() << '\n';
    out.close();
    if (!out) die("write failed: " + out_path);
    std::fprintf(stderr, "difftest_driver (%s): %llu records -> %s\n", kReference ? "reference" : "production",
                 static_cast<unsigned long long>(records), out_path.c_str());
    return 0;
}
