// Proof of the lifted leaf functions (tools/dolrecon/lift_cpp.py, runtime/tests/generated/dolrecon_lifted_leaves.cpp)
// against the ORIGINAL instructions of main.dol run by the reference interpreter (docs/DOLRECON.md "C++
// reconstruction"). The lifted code never sees the interpreter: it gets its own GuestMemory with the same DOL and the
// same randomized contents.
//
// Per function, kCases randomized states:
// - registers the function uses as a load / store base point into a scratch region whose words are themselves
//   mostly pointers into the scratch (so pointers read from memory can be followed); index registers get small
//   offsets; r1 the stack; r2 / r13 the small-data bases; other inputs edge values and random bits; CTR small when
//   the function loops on it; LR a sentinel return address.
// - the original runs until it returns to the sentinel. A case where it does not (a fault on a garbage pointer, a
//   hardware address) is INVALID input, counted, never compared.
// - every valid case compares all 32 GPRs, ps0 and ps1 of all 32 FPRs, CR, XER, CTR, LR, and every byte either side
//   wrote.
// Result per function: verified (>= kMinValid valid cases, 0 differences) / FAILED (first difference printed) /
// unverified (too few valid cases). Exit 1 if any function FAILED.
//
// Usage: dolrecon_lift_tests <main.dol> [results.csv]

#include "dolrecon_lift.h"
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace gekko;

using dolrecon::State;

constexpr u32 kScratch = 0x81100000u, kScratchSize = 0x20000u, kScratchMid = kScratch + kScratchSize / 2u;
constexpr u32 kStack = 0x817E0000u, kStackLow = 0x817D0000u;
constexpr u32 kReturn = 0x817FF000u;
// Up to kAttempts randomized states per function, stopping at kWantValid valid ones; >= kMinValid valid = verified.
constexpr int kAttempts = 256, kWantValid = 32, kMinValid = 8;
constexpr std::size_t kTraceCapacity = 4096;
constexpr u32 kMemLo = 0x80000000u, kMemHi = 0x81800000u;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void load_dol(GuestMemory& memory, const std::vector<u8>& dol) {
    const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
    for (unsigned i = 0; i < 18; ++i) {
        const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
        require(u64(offset) + size <= dol.size(), "invalid DOL section");
        if (size) memory.write_block(address, dol.data() + offset, size);
    }
}

// The lifted side's memory: the same GuestMemory API, with its own write record and a range check (an access outside
// MEM1 by the lifted code is a difference in itself - the original returned cleanly).
struct LiftedMem final : dolrecon::Mem {
    GuestMemory& g;
    std::vector<std::pair<u32, u32>> writes;
    bool out_of_range = false;
    u32 bad_address = 0, bad_size = 0;
    void refuse(u32 a, u32 n) {
        if (!out_of_range) {
            bad_address = a;
            bad_size = n;
        }
        out_of_range = true;
    }
    explicit LiftedMem(GuestMemory& memory) : g(memory) {}
    bool ok(u32 a, u32 n) {
        if (a >= gekko::kLockedCacheBase && u64(a) + n <= u64(gekko::kLockedCacheBase) + gekko::kLockedCacheSize)
            return true;  // the locked cache (dcbz_l, psq through 0xE0000000) is a mapped region on both sides
        if (a < kMemLo || u64(a) + n > u64(kMemHi)) {
            refuse(a, n);
            return false;
        }
        return true;
    }
    // Checked accesses: GuestMemory::read / write return false for a range that is not mapped and committed.
    u64 rd(u32 a, u32 n) {
        u8 b[8] = {};
        if (!ok(a, n)) return 0;
        if (!g.read(GcAddr{a}, b, n)) {
            refuse(a, n);
            return 0;
        }
        u64 v = 0;
        for (u32 i = 0; i < n; ++i) v = (v << 8) | b[i];
        return v;
    }
    void wr(u32 a, u32 n, u64 v) {
        u8 b[8];
        for (u32 i = 0; i < n; ++i) b[i] = static_cast<u8>(v >> (8 * (n - 1 - i)));
        if (!ok(a, n)) return;
        if (!g.write(GcAddr{a}, b, n)) {
            refuse(a, n);
            return;
        }
        writes.push_back({a, n});
    }
    u8 r8(u32 a) override { return static_cast<u8>(rd(a, 1)); }
    u16 r16(u32 a) override { return static_cast<u16>(rd(a, 2)); }
    u32 r32(u32 a) override { return static_cast<u32>(rd(a, 4)); }
    u64 r64(u32 a) override { return rd(a, 8); }
    void w8(u32 a, u8 v) override { wr(a, 1, v); }
    void w16(u32 a, u16 v) override { wr(a, 2, v); }
    void w32(u32 a, u32 v) override { wr(a, 4, v); }
    void w64(u32 a, u64 v) override { wr(a, 8, v); }
};

struct Harness {
    GuestMemory original_memory, lifted_memory;
    // One interpreter per function (fresh_cpu): no machine state can carry from one function's cases to the next.
    std::unique_ptr<Cpu> cpu_owner = std::make_unique<Cpu>(original_memory);
    Cpu& cpu_ref() { return *cpu_owner; }
    void fresh_cpu() {
        cpu_owner.reset();
        cpu_owner = std::make_unique<Cpu>(original_memory);
        have_clean = false;
    }
    bool recording = false;
    bool original_touched_hardware = false;  // an access outside MEM1 RAM: device registers, not comparable
    std::vector<std::pair<u32, u32>> original_writes;
    std::mt19937_64 rng{0x5EED2026u};

    explicit Harness(const std::vector<u8>& dol, const std::vector<u8>* mem1 = nullptr) {
        // Every region committed in full in both memories (zero-filled), so both sides see the same RAM - the
        // interpreter would otherwise commit pages on first touch while the lifted side's checked accesses refuse them.
        for (GuestMemory* m : {&original_memory, &lifted_memory}) {
            const auto regions = m->regions();
            for (const auto& r : regions) require(m->commit(r.base, r.size), "cannot commit region " + r.name);
            // A zero-filled guard after the end of RAM: an unaligned access straddling 0x81800000 on random input then
            // reads the same zeros on both sides instead of running off the host reservation.
            require(m->map(kMemHi, 0x10000u, 0x10000u, GuestMemory::RegionKind::Arena, "harness guard"),
                    "cannot map the guard region");
            // --mem1: a real MEM1 image of the original game (globals hold live pointers into real objects); the DOL
            // otherwise (its .bss is zero, so a function reading a global object pointer meets null).
            if (mem1 != nullptr)
                m->write_block(kMemLo, mem1->data(), mem1->size());
            else
                load_dol(*m, dol);
        }
        // The pristine image (code included): every function's test starts from it in BOTH memories. Without this a
        // previous function's random cases can store through a pointer into .text and the interpreter would then run
        // damaged original instructions while the lifted C++ (compiled from the clean DOL) does not - a test-order
        // dependent false difference (found 2026-10-31: fn_800DDD0C's cases left fn_800DE374 failing).
        for (const auto& r : original_memory.regions()) {
            if ((r.base < kMemLo || r.base >= kMemHi + 0x10000u) && r.base != gekko::kLockedCacheBase) continue;
            const u8* a = original_memory.peek(GcAddr{r.base}, r.size);
            require(a != nullptr, "cannot snapshot region " + r.name);
            pristine.push_back({r.base, std::vector<u8>(a, a + r.size)});
        }
        for (unsigned i = 0; i < 7; ++i) {
            const u32 address = load_be32(dol.data() + 0x48 + i * 4), size = load_be32(dol.data() + 0x90 + i * 4);
            if (size) text_ranges.push_back({address, address + size});
        }
        original_memory.set_access_trace([this](u32 address, std::size_t size, bool writing) {
            if (!recording) return;
            if (address < kMemLo || address + size > kMemHi) original_touched_hardware = true;
            if (writing) original_writes.push_back({address, static_cast<u32>(size)});
        });
    }

    u32 rnd32() { return static_cast<u32>(rng()); }

    // Identical random contents in both memories: scratch words are 50 % pointers into the scratch. The whole scratch
    // and stack once per function; per case the window the case's base pointers start in (kScratchMid +- 4 KiB) and
    // the top of the stack.
    void fill(u32 base, u32 size) {
        std::vector<u8> bytes(size);
        for (u32 i = 0; i + 4 <= size; i += 4) {
            const u32 v = (rng() & 1) ? (kScratch + (rnd32() % (kScratchSize - 0x100u))) & ~7u : rnd32();
            store_be32(bytes.data() + i, v);
        }
        original_memory.write_block(base, bytes.data(), bytes.size());
        lifted_memory.write_block(base, bytes.data(), bytes.size());
    }
    void fill_all() {
        fill(kScratch, kScratchSize);
        fill(kStackLow, kStack + 0x100u - kStackLow);
    }
    void fill_case() {
        fill(kScratchMid - 0x1400u, 0x2800u);
        fill(kStack - 0x400u, 0x500u);
    }

    u32 edge_or_random() {
        static const u32 edges[] = {0u, 1u, 2u, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFu, 0x10000u,
                                    0x8000u, 31u, 32u, 33u, 63u, 64u};
        const u64 pick = rng() % 4;
        if (pick == 0) return edges[rng() % (sizeof edges / sizeof edges[0])];
        if (pick == 1) return static_cast<u32>(rng() % 256);
        return rnd32();
    }

    State random_state(const dolrecon::Lifted& fn) {
        State s{};
        for (unsigned n = 0; n < 32; ++n) s.r[n] = edge_or_random();
        for (unsigned n = 0; n < 32; ++n) {
            if (fn.base_regs & (1u << n)) s.r[n] = (kScratchMid + (rnd32() % 0x2000u) - 0x1000u) & ~7u;
            if (fn.index_regs & (1u << n)) s.r[n] = (rnd32() % 0x400u) & ~3u;
        }
        s.r[1] = kStack;
        s.r[2] = dolrecon::kSda2Base;
        s.r[13] = dolrecon::kSdaBase;
        static const u64 fedges[] = {0ull, 0x8000000000000000ull, 0x3FF0000000000000ull, 0x7FF0000000000000ull,
                                     0x7FF8000000000000ull, 0x7FF4000000000001ull, 0x0000000000000001ull,
                                     0x380FFFFFE0000000ull, 0x47EFFFFFE0000000ull};
        for (unsigned n = 0; n < 32; ++n) {
            s.f[n] = (rng() % 3 == 0) ? fedges[rng() % (sizeof fedges / sizeof fedges[0])] : rng();
            s.p[n] = rng();
        }
        s.cr = rnd32();
        s.xer = (rnd32() & 0xE000007Fu);
        s.ctr = fn.uses_ctr_loop ? static_cast<u32>(rng() % 48u) : rnd32();
        s.lr = kReturn;
        // FPSCR: random bits, NI on in half the cases (the game runs with NI = 1)
        s.fpscr = (rnd32() & ~0x4u) | ((rng() & 1) ? 0x4u : 0u);
        // GQRs: GQR0 = 0 (the OS's value), the others random types and scales
        s.gqr[0] = 0;
        for (unsigned i = 1; i < 8; ++i) s.gqr[i] = rnd32() & 0x3F073F07u;
        s.fault = 0;
        s.fault_address = 0;
        s.budget = 4000000;
        return s;
    }

    // Runs the original; false when it did not return cleanly (invalid input).
    // Cpu::reset() restores SPRs, segment registers, MSR and FPSCR itself (cpu.cpp reset; checked with GPT-6-Astra,
    // 2026-10-31); a fresh Cpu per function (fresh_cpu) adds isolation on top. The pristine image per function and the
    // code repair per case (repair_code) are what keep the ORIGINAL instructions intact.
    bool have_clean = false;

    bool run_original(u32 entry, const State& in, State& out) {
        cpu_ref().reset();
        // As production runs: an access with nothing mapped stops the machine (off, the reference core reads 0 and
        // drops the store, and a case reading through a garbage pointer would look like a clean return).
        cpu_ref().set_unmapped_access_stops(true);
        cpu_ref().set_msr(cpu_ref().msr() | kMsrFP);
        for (unsigned n = 0; n < 32; ++n) {
            cpu_ref().gpr[n] = in.r[n];
            cpu_ref().fpr[n].bits[0] = in.f[n];
            cpu_ref().fpr[n].bits[1] = in.p[n];
        }
        cpu_ref().cr = in.cr;
        cpu_ref().xer = in.xer;
        cpu_ref().ctr = in.ctr;
        cpu_ref().lr = in.lr;
        cpu_ref().fpscr = in.fpscr;
        for (unsigned i = 0; i < 8; ++i) cpu_ref().spr[gekko::SPR_GQR0 + i] = in.gqr[i];
        cpu_ref().pc = entry;
        cpu_ref().npc = entry + 4;
        original_writes.clear();
        original_touched_hardware = false;
        original_trace.clear();
        host_fault = false;
        recording = true;
        // The entry itself is the first event, as the lifted side records it.
        record_entry(entry);
        if (!run_guarded()) host_fault = true;
        recording = false;
        wrote_code = false;
        repair_code(original_writes);
        if (wrote_code || host_fault || cpu_ref().pc != kReturn || cpu_ref().stopped() || original_touched_hardware)
            return false;
        for (unsigned n = 0; n < 32; ++n) {
            out.r[n] = cpu_ref().gpr[n];
            out.f[n] = cpu_ref().fpr[n].bits[0];
            out.p[n] = cpu_ref().fpr[n].bits[1];
        }
        out.cr = cpu_ref().cr;
        out.xer = cpu_ref().xer;
        out.ctr = cpu_ref().ctr;
        out.lr = cpu_ref().lr;
        out.fpscr = cpu_ref().fpscr;
        for (unsigned i = 0; i < 8; ++i) out.gqr[i] = cpu_ref().spr[gekko::SPR_GQR0 + i];
        return true;
    }

    // ---- the interpreter's side of the call trace ----------------------------------------------------------------
    std::vector<dolrecon::TraceEvent> original_trace;
    bool host_fault = false;
    void record_entry(u32 pc) {
        if (original_trace.size() >= kTraceCapacity) return;
        dolrecon::TraceEvent e{};
        e.address = pc;
        e.r1 = cpu_ref().gpr[1];
        for (unsigned i = 0; i < 8; ++i) e.r[i] = cpu_ref().gpr[3 + i];
        e.f1 = cpu_ref().fpr[1].bits[0];
        original_trace.push_back(e);
    }
    static bool is_entry(u32 pc) {
        std::size_t lo = 0, hi = dolrecon::kLiftedCount;
        while (lo < hi) {
            const std::size_t mid = (lo + hi) / 2;
            if (dolrecon::kLifted[mid].address < pc) lo = mid + 1; else hi = mid;
        }
        return lo < dolrecon::kLiftedCount && dolrecon::kLifted[lo].address == pc;
    }
    // Coverage: which instructions of the function under test [cover_lo, cover_hi) the original executed in this case.
    u32 cover_lo = 0, cover_hi = 0;
    std::vector<u8> case_cover;
    void step_loop() {
        unsigned steps = 0;
        u32 prev = cpu_ref().pc;
        if (prev >= cover_lo && prev < cover_hi) case_cover[(prev - cover_lo) / 4u] = 1;
        while (cpu_ref().pc != kReturn && !cpu_ref().stopped() && steps < 200000u) {
            cpu_ref().run_steps(1);
            ++steps;
            if (cpu_ref().pc < 0x80003000u || cpu_ref().pc >= 0x80600000u) break;  // an exception vector or a wild jump
            // entered by a branch (not by falling through into a labelled second entry)
            if (cpu_ref().pc != prev + 4u && cpu_ref().pc != kReturn && is_entry(cpu_ref().pc)) record_entry(cpu_ref().pc);
            prev = cpu_ref().pc;
            if (prev >= cover_lo && prev < cover_hi) case_cover[(prev - cover_lo) / 4u] = 1;
        }
    }
    // A host access fault inside the reference core (an ORIGINAL access straddling the top of the 32-bit guest window
    // on random input; the core's fault handler refuses it) ends this case as invalid input instead of the process.
    bool run_guarded() {
#if defined(_MSC_VER)
        __try {
            step_loop();
        } __except (GetExceptionCode() == 0xC0000005u ? 1 : 0) {
            return false;
        }
        return true;
#else
        step_loop();
        return true;
#endif
    }

    std::vector<std::pair<u32, std::vector<u8>>> pristine;
    std::vector<std::pair<u32, u32>> text_ranges;  // the DOL's text sections [start, end)
    bool wrote_code = false;
    // A case whose original stored into the code (a random pointer into .text): invalid input. The code bytes go
    // back to pristine in both memories at once, so the next case runs the true original instructions again.
    void repair_code(const std::vector<std::pair<u32, u32>>& writes) {
        for (const auto& [a, n] : writes)
            for (const auto& [lo, hi] : text_ranges)
                if (a < hi && a + n > lo) {
                    wrote_code = true;
                    for (const auto& [base, bytes] : pristine)
                        if (a >= base && a + n <= base + bytes.size()) {
                            original_memory.write(GcAddr{a}, bytes.data() + (a - base), n);
                            lifted_memory.write(GcAddr{a}, bytes.data() + (a - base), n);
                        }
                }
    }
    void restore_pristine() {
        for (const auto& [base, bytes] : pristine) {
            std::memcpy(original_memory.span(GcAddr{base}, bytes.size()), bytes.data(), bytes.size());
            std::memcpy(lifted_memory.span(GcAddr{base}, bytes.size()), bytes.data(), bytes.size());
        }
    }

    // Harness integrity: the two memories must be identical between functions. Returns the first differing address
    // (and makes them identical again from the original's side), or 0 when they already are.
    u32 check_and_resync_all() {
        u32 first = 0;
        for (const auto& r : original_memory.regions()) {
            if ((r.base < kMemLo || r.base >= kMemHi + 0x10000u) && r.base != gekko::kLockedCacheBase) continue;
            const u8* a = original_memory.peek(GcAddr{r.base}, r.size);
            u8* b = lifted_memory.span(GcAddr{r.base}, r.size);
            if (a == nullptr || b == nullptr) continue;
            if (std::memcmp(a, b, r.size) != 0) {
                for (u32 i = 0; i < r.size; ++i)
                    if (a[i] != b[i]) {
                        if (first == 0) first = r.base + i;
                        break;
                    }
                std::memcpy(b, a, r.size);
            }
        }
        return first;
    }

    // Copies whatever the original wrote into the lifted memory too, so the two stay identical after a case.
    void resync(const std::vector<std::pair<u32, u32>>& a, const std::vector<std::pair<u32, u32>>& b) {
        for (const auto* list : {&a, &b})
            for (const auto& [address, size] : *list) {
                u8 buffer[8];
                // RAM and the guard region after it (an invalid case can store into the guard: found 2026-10-31 as
                // ten "memories differed" reports at 0x818xxxxx); device registers have no RAM behind them.
                const bool lc = address >= gekko::kLockedCacheBase &&
                                u64(address) + size <= u64(gekko::kLockedCacheBase) + gekko::kLockedCacheSize;
                if (!lc && (address < kMemLo || address + size > kMemHi + 0x10000u)) continue;
                if (original_memory.read(GcAddr{address}, buffer, size))
                    lifted_memory.write(GcAddr{address}, buffer, size);
            }
    }
    // A host-side byte read that cannot fault: false when nothing is mapped there.
    static bool byte_at(GuestMemory& m, u32 address, u8& value) {
        const bool lc = address >= gekko::kLockedCacheBase && address < gekko::kLockedCacheBase + gekko::kLockedCacheSize;
        return (lc || (address >= kMemLo && address < kMemHi + 0x10000u)) && m.read(GcAddr{address}, &value, 1);
    }
};

std::string hex(u64 v) {
    char t[24];
    std::snprintf(t, sizeof t, "%llX", static_cast<unsigned long long>(v));
    return t;
}

// The first call-trace event where the original and the lifted runs differ (function entered, or the state it was
// entered with): the place behaviour first parted. Empty when the traces agree (the difference is then in the
// function's own body after its last call).
std::string first_divergence(const std::vector<dolrecon::TraceEvent>& a, const dolrecon::TraceEvent* b, std::size_t nb) {
    const std::size_t n = std::min(a.size(), nb);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& x = a[i];
        const auto& y = b[i];
        std::string what;
        if (x.address != y.address) {
            what = "entered " + hex(x.address) + " (original) vs " + hex(y.address) + " (lifted)";
        } else if (x.r1 != y.r1) {
            what = "r1 differs entering " + hex(x.address);
        } else {
            for (unsigned k = 0; k < 8 && what.empty(); ++k)
                if (x.r[k] != y.r[k])
                    what = "r" + std::to_string(3 + k) + " " + hex(x.r[k]) + " vs " + hex(y.r[k]) + " entering " +
                           hex(x.address);
            if (what.empty() && x.f1 != y.f1) what = "f1 differs entering " + hex(x.address);
        }
        if (!what.empty())
            return " | first divergence at call event " + std::to_string(i) + ": " + what +
                   (i > 0 ? " (previous event: " + hex(a[i - 1].address) + ")" : "");
    }
    if (a.size() != nb)
        return " | call traces differ in length (" + std::to_string(a.size()) + " vs " + std::to_string(nb) +
               "), first " + std::to_string(n) + " events agree";
    return " | call traces agree (" + std::to_string(n) + " events): the difference is in the function's own body";
}

struct Outcome {
    bool skipped = false;
    u32 desync = 0;  // harness integrity: the memories differed after this function (first address)
    int valid = 0, invalid = 0, hardware = 0, unlifted_callee = 0;
    u32 covered = 0, instructions = 0;  // instructions of the function the original executed in VALID cases
    std::string failure;
};

Outcome test_one(Harness& h, const dolrecon::Lifted& fn) {
    Outcome o;
    h.rng.seed(0x5EED2026u ^ (u64(fn.address) << 16));
    h.fresh_cpu();
    h.restore_pristine();  // every function starts from the same image: results do not depend on test order
    h.cover_lo = fn.address;
    h.cover_hi = fn.address + fn.instructions * 4u;
    std::vector<u8> cover(fn.instructions, 0);
    o.instructions = fn.instructions;  // per function: results do not depend on thread scheduling
    h.fill_all();
    std::vector<dolrecon::TraceEvent> lifted_trace(kTraceCapacity);
    for (int c = 0; c < kAttempts && o.failure.empty() && o.valid < kWantValid; ++c) {
        h.fill_case();
        h.case_cover.assign(fn.instructions, 0);
        const State in = h.random_state(fn);
        State want{};
        if (!h.run_original(fn.address, in, want)) {
            ++o.invalid;
            if (h.original_touched_hardware) ++o.hardware;
            h.resync(h.original_writes, {});
            continue;
        }
        LiftedMem mem(h.lifted_memory);
        State got = in;
        got.trace_events = lifted_trace.data();
        got.trace_count = 0;
        got.trace_capacity = kTraceCapacity;
        fn.fn(mem, got);
        // A lifted run that stopped for a reason that is not semantics (no lifted callee at an address the original
        // reached, the budget) is not comparable: counted, never passed. A return to somewhere other than the entry LR
        // while the original returned normally IS a difference.
        if (got.fault == dolrecon::kFaultNoTarget || got.fault == dolrecon::kFaultBudget) {
            ++o.invalid;
            if (got.fault == dolrecon::kFaultNoTarget) ++o.unlifted_callee;
            h.resync(h.original_writes, mem.writes);
            continue;
        }
        if (got.fault == dolrecon::kFaultReturnElsewhere) {
            o.failure = "lifted returned to " + hex(got.fault_address) + ", the original returned normally";
            break;
        }
        ++o.valid;
        for (u32 k = 0; k < fn.instructions; ++k) cover[k] |= h.case_cover[k];
        const auto diff = [&](const std::string& what, u64 w, u64 g) {
            if (o.failure.empty() && w != g) o.failure = what + " original " + hex(w) + " lifted " + hex(g);
        };
        if (mem.out_of_range && o.failure.empty())
            o.failure = "lifted access refused at " + hex(mem.bad_address) + " size " + std::to_string(mem.bad_size);
        for (unsigned n = 0; n < 32; ++n) {
            diff("r" + std::to_string(n), want.r[n], got.r[n]);
            diff("f" + std::to_string(n) + ".ps0", want.f[n], got.f[n]);
            diff("f" + std::to_string(n) + ".ps1", want.p[n], got.p[n]);
        }
        diff("cr", want.cr, got.cr);
        diff("xer", want.xer, got.xer);
        diff("ctr", want.ctr, got.ctr);
        diff("lr", want.lr, got.lr);
        diff("fpscr", want.fpscr, got.fpscr);
        for (unsigned i = 0; i < 8; ++i) diff("gqr" + std::to_string(i), want.gqr[i], got.gqr[i]);
        std::map<u32, int> touched;
        for (const auto& [a, n] : h.original_writes)
            for (u32 k = 0; k < n; ++k) touched[a + k] |= 1;
        for (const auto& [a, n] : mem.writes)
            for (u32 k = 0; k < n; ++k) touched[a + k] |= 2;
        for (const auto& [a, who] : touched) {
            if (!o.failure.empty()) break;
            u8 w = 0, g = 0;
            const bool wok = Harness::byte_at(h.original_memory, a, w), gok = Harness::byte_at(h.lifted_memory, a, g);
            if (!wok || !gok) {
                o.failure = "byte " + hex(a) + " written but not readable back";
            } else if (who != 3) {
                o.failure = "byte " + hex(a) + (who == 1 ? " written only by the original" : " written only by lifted");
                if (w == g) o.failure += " (same value)";
            } else if (w != g) {
                o.failure = "byte " + hex(a) + " original " + hex(w) + " lifted " + hex(g);
            }
        }
        if (!o.failure.empty()) o.failure += first_divergence(h.original_trace, lifted_trace.data(), got.trace_count);
        h.resync(h.original_writes, mem.writes);
    }
    for (const u8 c : cover) o.covered += c;
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 2, "usage: dolrecon_lift_tests <main.dol> [results.csv|-] [threads] [--skip file] "
                           "[--range from to]");
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        // --skip <file>: functions whose ORIGINAL code faulted the reference interpreter on randomized input (recorded
        // with evidence by tools/dolrecon/lift_verify.py); reported as unverified, never as verified.
        // --range <from> <to>: one process, kLifted[from..to), "BEGIN i" before and "RESULT ..." after each function,
        // flushed (the driver's crash recovery).
        std::vector<u32> skip;
        std::vector<u8> mem1;
        std::size_t range_from = 0, range_to = 0;
        bool range = false;
        std::vector<std::string> positional;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--skip" && i + 1 < argc) {
                std::ifstream f(argv[++i]);
                std::string line;
                while (std::getline(f, line))
                    if (!line.empty() && line[0] != '#') skip.push_back(static_cast<u32>(std::stoul(line, nullptr, 16)));
            } else if (a == "--mem1" && i + 1 < argc) {
                std::ifstream f(argv[++i], std::ios::binary);
                mem1.assign(std::istreambuf_iterator<char>(f), {});
                require(mem1.size() == 0x1800000u, "--mem1 wants a 24 MiB MEM1 image");
            } else if (a == "--range" && i + 2 < argc) {
                range = true;
                range_from = std::stoul(argv[++i]);
                range_to = std::min<std::size_t>(std::stoul(argv[++i]), dolrecon::kLiftedCount);
            } else {
                positional.push_back(a);
            }
        }
        if (range) {
            Harness h(dol, mem1.empty() ? nullptr : &mem1);
            for (std::size_t i = range_from; i < range_to; ++i) {
                const dolrecon::Lifted& fn = dolrecon::kLifted[i];
                std::printf("BEGIN %zu\n", i);
                std::fflush(stdout);
                Outcome o = test_one(h, fn);
                if (const u32 d = h.check_and_resync_all()) o.desync = d;
                const char* status = !o.failure.empty() ? "FAILED" : o.valid >= kMinValid ? "verified"
                                     : o.unlifted_callee > 0 ? "unverified_unlifted_callee"
                                     : o.hardware > 0 ? "unverified_hardware" : "unverified";
                const std::string detail =
                    "cov " + std::to_string(o.covered) + "/" + std::to_string(o.instructions) + (o.failure.empty() ? "" : " ") +
                    o.failure + (o.desync ? " [harness: memories differed after this function at " + hex(o.desync) + "]"
                                          : std::string());
                std::printf("RESULT %zu,%08X,%s,%u,%d,%d,%s,\"%s\"\n", i, fn.address, fn.name, fn.instructions,
                            o.valid, o.invalid, status, detail.c_str());
                std::fflush(stdout);
            }
            return 0;
        }
        const unsigned threads = positional.size() >= 3 ? static_cast<unsigned>(std::stoul(positional[2]))
                                                        : std::min(16u, std::max(1u, std::thread::hardware_concurrency()));
        std::vector<Outcome> outcomes(dolrecon::kLiftedCount);
        std::atomic<std::size_t> next{0};
        std::vector<std::thread> pool;
        std::mutex error_mutex;
        std::string worker_error;
        for (unsigned t = 0; t < threads; ++t)
            pool.emplace_back([&] {
                try {
                    Harness h(dol, mem1.empty() ? nullptr : &mem1);  // each worker: its own interpreter and both memories
                    for (std::size_t i = next++; i < dolrecon::kLiftedCount; i = next++) {
                        if (std::find(skip.begin(), skip.end(), dolrecon::kLifted[i].address) != skip.end()) {
                            outcomes[i].skipped = true;
                            continue;
                        }
                        outcomes[i] = test_one(h, dolrecon::kLifted[i]);
                        if (const u32 d = h.check_and_resync_all()) outcomes[i].desync = d;
                    }
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lock(error_mutex);
                    worker_error = e.what();
                }
            });
        for (auto& t : pool) t.join();
        require(worker_error.empty(), worker_error);
        std::ofstream results;
        if (positional.size() >= 2 && positional[1] != "-") {
            results.open(positional[1]);
            results << "address,name,instructions,valid_cases,invalid_cases,status,detail\n";
        }
        u64 verified = 0, failed = 0, unverified = 0, unverified_hardware = 0, unverified_faulted = 0, compared = 0;
        for (std::size_t i = 0; i < dolrecon::kLiftedCount; ++i) {
            const dolrecon::Lifted& fn = dolrecon::kLifted[i];
            const Outcome& o = outcomes[i];
            compared += static_cast<u64>(o.valid);
            const char* status = o.skipped ? "unverified_original_faulted"
                                 : !o.failure.empty() ? "FAILED"
                                 : o.valid >= kMinValid ? "verified"
                                 : o.unlifted_callee > 0 ? "unverified_unlifted_callee"
                                 : o.hardware > 0 ? "unverified_hardware"
                                                  : "unverified";
            if (o.skipped) {
                ++unverified_faulted;
            } else if (!o.failure.empty()) {
                ++failed;
                std::printf("FAILED %08X %s: %s\n", fn.address, fn.name, o.failure.c_str());
            } else if (o.valid >= kMinValid) {
                ++verified;
            } else if (o.hardware > 0) {
                ++unverified_hardware;
            } else {
                ++unverified;
            }
            if (results.is_open())
                results << hex(fn.address) << ',' << fn.name << ',' << fn.instructions << ',' << o.valid << ','
                        << o.invalid << ',' << status << ",\"" << o.failure << "\"\n";
        }
        std::printf("dolrecon_lift_tests: %zu lifted functions: %llu verified, %llu FAILED, %llu unverified "
                    "(too few valid inputs), %llu unverified (device registers), %llu unverified (original faulted the interpreter); %llu cases compared, %u threads\n",
                    dolrecon::kLiftedCount, static_cast<unsigned long long>(verified),
                    static_cast<unsigned long long>(failed), static_cast<unsigned long long>(unverified),
                    static_cast<unsigned long long>(unverified_hardware), static_cast<unsigned long long>(unverified_faulted),
                    static_cast<unsigned long long>(compared),
                    threads);
        return failed ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dolrecon_lift_tests: %s\n", e.what());
        return 2;
    }
}
