// F1c long-run canary driver (test-only).
//
// Links the F1c generated objects (real ppcrecomp output, shard flags), the PRODUCTION shadow_runtime, and
// f1c_guard_alloc.cpp, which replaces this executable's global operator new/delete: the Cpu object, the
// GuestMemory (its 2 MiB page table and region bookkeeping), the RecompiledCode object and its slot arrays, the
// HLE hook vector - every heap block - sits in a guard-paged or canary-bracketed allocation. No layout of
// cpu.h/guest_memory.h is touched; guest code runs through Cpu::run().
//
// Native time (docs/native-time-20261005/DESIGN.md): nothing counts instructions, guest code runs in zero game
// time and the game clock moves only where a wait delivers an event. The test device below therefore acts at
// host callbacks the native runtime really makes - HLE hooks at translated entry thunks and at a dispatcher
// arrival - on a cadence of guest progress, never of instructions.
//
// Phases (one thread with a 1 GiB stack reserve; one Cpu for the whole process, never reset between phases):
//   A  shapes   every gen_f1c.py loop shape, `--shape-reps` times each with `--shape-iterations` iterations,
//               MSR[EE] alternating: every run returns to the sentinel with r3 == the iteration count and r1
//               restored, all canaries hold after each run, and the host stack watermark of each shape must be
//               identical in every repetition (drift). The watermark is sampled by declining HLE probes at the
//               shape's record heads, the shared translated leaf and its served callee (the f1c_stack_driver
//               `hook` sampler; the retire_block service tick that used to sample it no longer exists). A loop
//               the generator compiled into one native body that makes no host call at all (the tiny intra_t,
//               xrec_direct_t, eof_fall_t, bctr_local_t and bclr_top_t today) reaches no probe: no host frame
//               is created inside it, so there is nothing to observe and that drift sub-check is retired for it -
//               the shape is listed as "unsampled" in the phase record, never counted as drift-checked. A shape
//               gen_f1c.py builds with a host call in every iteration - every game-shaped `_g`/`_b` variant (an
//               indirect call to the leaf or the next record) and every shape with a served callee - must be
//               sampled in every repetition, and a shape sampled in some repetitions only fails.
//   B1 longrun  the canary `longrun` program for `--outer` outer iterations with injected external interrupts,
//               decrementer edges delivered as the game-clock scheduler delivers them, sc round trips, EE
//               toggling and the RAM poll idle loop. The device is a declining HLE probe on the translated leaf
//               the program calls twice per inner-A iteration (bl and bctrl); one device service per call:
//                 - external line: acked (dropped) once the guest's external handler counted it (r23 changed),
//                   asserted every `--irq-every` services while not asserted;
//                 - decrementer: every `--dec-every` services, if no edge is still latched, the clock moves to
//                   the guest-armed GameEvent::Decrementer deadline (the guest's mtdec reload) and the edge is
//                   latched (Cpu::latch_decrementer) - exactly what a wait point does when that event is next;
//                 - every 2^22 services: the periodic invariant checks.
//               The idle loop's device completes the polled operation when the guest arrives at the loop head
//               (a declining hook there, reached from the dispatcher after every sc, sets the word).
//               M1 (docs/memory-architecture-20261006): the exceptions go through the native OS exception
//               entry, which this driver installs as the OS does; the handlers are the fixture's translated
//               rows (f1c::kExternalHandler, f1c::kDecrementerHandler) and return through the OSContext; the
//               system call is PPCSync's head, performed natively.
// Checks after every run and every 2^22 device services inside B1: guard allocator canaries (every live block),
//   Cpu pointer fields (the GuestMemory, its page table, the recompiled table) unchanged, the HLE slot table
//   byte-identical to the rebuild_hle_slots pattern, every page-table entry the host bytes of its region (or
//   null where nothing is committed), the active table pointer matching the MSR translation mode, the
//   recompiled lookup table (every slot and range) unchanged, the stack guard page present, the dispatcher-top
//   host stack depth (the dispatch that resumes after sc) constant; and at the end of B1 exact accounting: the game clock moved only at the device's deliveries
//   (now - start == the ticks they advanced, each one DEC_RELOAD + 1 after the guest's reload), the time base
//   and the decrementer are its views (TB delta == clock delta; DEC == the reload, or -1 with an edge still
//   latched), every delivered edge taken or still latched, device services == leaf calls (2 * outer * INNER_A),
//   idle-loop completions == outer, exception census == guest handler counters, fallback totals ==
//   interpreter_exits, guest loop counters == their analytic values, zero non-native executions.
//
// usage: f1c_canary_driver --out report.json [--outer N] [--shape-reps N] [--shape-iterations N]
//                          [--irq-every N] [--dec-every N] [--phases A,B1]
//
// 2026-10-05: phase C and the store-table / guard checks were deleted with the Bus guard machinery and the
// Cpu store and tagged page tables they exercised (legacy GX raster retirement); loads and stores now share
// the one memo table checked here.
//
// 2026-10-06 (native time): phases B2/B2x (wrap) and B3 (coalesce charge width) are RETIRED. B2 drove
// total_instructions, the instruction-derived time base and DEC and the hardware-service ledgers past 2^32
// through coalesced idle charges at service intervals 2^24 and 0xFFFFFFFF; B3 checked the retired count at the
// first hardware service of the pollwrap idle loop at intervals >= 2^32/3 (finding W1, the u32 charge width of
// coalesce_ram_poll). Every quantity they measured - the instruction counter, instruction-derived timers,
// Cpu::set_hardware_tick services and their `retired` argument, coalesced charges - no longer exists, so no
// observable contract remains; the per-service ledger (each service's `retired` == the instructions since the
// previous one) and the coalesced-iteration checks of B1 went with them. Asking for B2 or B3 is an error.
#include "gekko/os_exception_vector.h"
#include "gekko/cpu.h"
#include "gekko/cpu_diag.h"
#include "gekko/guest_memory.h"
#include "gekko/recomp.h"

#include "f1c_fixture.h"
#include "f1c_guard_alloc.h"
#include "f1c_host.h"

#include <intrin.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace gekko;

namespace {

struct Options {
    std::string out;
    u64 outer = 20000;
    u64 shape_reps = 6;
    u64 shape_iterations = 20000;
    // Device services (leaf calls) between external-line assertions and between decrementer deliveries: an
    // external interrupt about every 4 outer iterations and an edge about every 16 (each outer iteration makes
    // 2 * INNER_A = 4,000 leaf calls), the proportions the instruction-clock version of B1 produced. The edge
    // cadence is prime so it rarely meets an assertion: an edge then arrives with EE = 1 (taken right after the
    // leaf returns, R2) as well as with EE = 0 (taken at the next outer iteration's mtmsr, R4).
    u64 irq_every = 16384;
    u64 dec_every = 65521;
    std::set<std::string> phases{"A", "B1"};
};

constexpr u64 kCheckEvery = u64{1} << 22;  // device services between periodic invariant checks

std::string quote(const std::string& text) {
    std::string out = "\"";
    for (char ch : text) {
        if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
        else if (static_cast<unsigned char>(ch) < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", ch); out += b; }
        else out += ch;
    }
    return out + "\"";
}

std::string hex64(u64 v) {
    char b[24];
    std::snprintf(b, sizeof(b), "\"0x%llx\"", static_cast<unsigned long long>(v));
    return b;
}

GEKKO_NOINLINE std::uintptr_t probe_address() {
    return reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
}

// ---- report ------------------------------------------------------------------------------------------------
struct Report {
    std::vector<std::string> failures;
    std::vector<std::string> phases;
    u64 checks = 0;
    void fail(const std::string& what) {
        if (failures.size() < 400) failures.push_back(what);
    }
};

// ---- the machine under test --------------------------------------------------------------------------------
struct Machine {
    RecompiledCode* code = nullptr;
    GuestMemory* bus = nullptr;
    Cpu* cpu = nullptr;
    // The test device behind the hooks run() installs once (the HLE slot table must never change after the
    // invariants are captured, so the hooks stay installed and their behaviour follows the phase).
    enum class Mode { Off, Sample, Longrun };
    Mode mode = Mode::Off;
    std::uintptr_t lowest = ~std::uintptr_t{0};  // Sample: the deepest probe of the current run
    u64 samples = 0;
    std::function<void(Cpu&)> service;            // Longrun: one device service (the leaf probe)
    std::function<void()> poll_device;            // Longrun: the idle loop's device (the loop-head hook)
};

GEKKO_NOINLINE void sample_stack(Machine& m) {
    m.lowest = std::min(m.lowest, probe_address());
    ++m.samples;
}

// gen_f1c.py builds these shapes with a host call in every loop iteration: the game-shaped variants (`_g`, `_b`)
// make an indirect call (to the leaf, or to the next record) and the HLE shapes call their served callee.
bool makes_host_call_every_iteration(const f1c::Shape& s) {
    const std::string name = s.name;
    const bool game_shaped = name.size() > 2u && (name.compare(name.size() - 2u, 2u, "_g") == 0 ||
                                                  name.compare(name.size() - 2u, 2u, "_b") == 0);
    return game_shaped || s.hook != 0u;
}

// The translated record heads of shape `s` other than its entry (see f1c_stack_driver.cpp probe_addresses):
// only a record head has its own address as the suffix of the generated "<shape>_<k>_<ADDRESS>" name, and only
// a record head's entry thunk calls try_hle.
std::vector<u32> shape_record_heads(const f1c::Shape& s) {
    std::vector<u32> out;
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
        out.push_back(e.address);
    }
    return out;
}

struct PointerField {
    std::size_t offset;
    u64 value;
    const char* what;
};

struct Invariants {
    std::vector<PointerField> fields;
    std::size_t hle_slots_offset = ~std::size_t{0};
    std::vector<u8> hle_slots_bytes;
    std::vector<std::pair<u32, std::string>> hooks;
    u64 table_hash = 0;
    std::size_t table_bytes = 0;
    u32 table_low = 0, table_high = 0;
    // FM step 2 (EV 2026-10-20): the flat map replaced the page table. GuestMemory's window (Cpu::fm_window_)
    // and null window (Cpu::fm_null_window_) are constant fields; Cpu::fm_base_ is one of them, checked
    // against the MSR mode (the role active_pages_ had, C2 2026-09-28).
    std::uintptr_t host_pages = 0;  // GuestMemory::fm_window()
    std::uintptr_t null_window = 0;
    std::size_t active_pages_offset = ~std::size_t{0};  // Cpu::fm_base_
    std::uintptr_t depth_base = 0;
    bool top_seen = false;
    std::uintptr_t top_min = ~std::uintptr_t{0}, top_max = 0;
    u64 top_samples = 0;
};

u64 fnv(u64 h, u64 v) {
    for (int i = 0; i < 8; ++i) { h ^= (v >> (8 * i)) & 0xFFu; h *= 0x100000001B3ull; }
    return h;
}

u64 table_hash(const RecompiledCode& code) {
    u64 h = 0xCBF29CE484222325ull;
    for (u64 a = code.low(); a < code.high(); a += 4u) h = fnv(fnv(h, a), reinterpret_cast<u64>(code.lookup(static_cast<u32>(a))));
    for (std::size_t i = 0; i < recomp::kRangeCount; ++i) {
        const RecompiledCode::Range* r = code.find_range(recomp::kRanges[i].begin);
        h = fnv(h, reinterpret_cast<u64>(r));
        h = fnv(h, reinterpret_cast<u64>(r ? r->at : nullptr));
    }
    return h;
}

std::vector<u8> expected_hle_slots(const std::vector<std::pair<u32, std::string>>& hooks) {
    // Cpu::rebuild_hle_slots for a non-verifying, non-faulted machine (kHleSlotEmpty 1, kHleSlotAll 3).
    // The table is Cpu::kHleSlotCount (4096) words = 16 KiB since 2026-11-04 (it was 1024 words).
    constexpr std::size_t kSlots = 4096u;
    std::vector<u32> slots(kSlots, 1u);
    for (const auto& [address, name] : hooks) {
        u32& slot = slots[(address >> 2) & (kSlots - 1u)];
        slot = (slot == 1u && address != 1u && address != 3u) ? address : 3u;
    }
    std::vector<u8> bytes(kSlots * sizeof(u32));
    std::memcpy(bytes.data(), slots.data(), bytes.size());
    return bytes;
}

// The object must be a live tracked allocation of exactly sizeof(T), in the layout the allocator selects for its
// size and alignment: guard-paged at or above the threshold, a canary-bracketed heap block below it (the Bus since
// its guard bitmaps were deleted, RecompiledCode always).
template <class T>
void require_tracked_object(const T* object, const char* name, Report& rep) {
    std::size_t size = 0;
    bool guarded = false;
    const bool expect_guarded = f1c::guard::guard_paged_layout(sizeof(T), alignof(T));
    if (!f1c::guard::tracked_block(reinterpret_cast<std::uintptr_t>(object), size, guarded) || size != sizeof(T) ||
        guarded != expect_guarded)
        rep.fail(std::string("the ") + name + " object is not a " + (expect_guarded ? "guard-paged" : "canary-bracketed") +
                 " allocation of sizeof(" + name + ")");
}

void capture_invariants(Machine& m, Invariants& inv, Report& rep) {
    const auto* object = reinterpret_cast<const u8*>(m.cpu);
    const std::size_t size = sizeof(Cpu);
    require_tracked_object(m.cpu, "Cpu", rep);
    require_tracked_object(m.bus, "GuestMemory", rep);
    require_tracked_object(m.code, "RecompiledCode", rep);
    const std::uintptr_t window = reinterpret_cast<std::uintptr_t>(m.bus->fm_window());
    const std::uintptr_t null_window = reinterpret_cast<std::uintptr_t>(m.bus->fm_null_window());
    std::vector<std::pair<std::size_t, u64>> window_refs, null_refs;
    for (std::size_t off = 0; off + 8u <= size; off += 8u) {
        u64 v = 0;
        std::memcpy(&v, object + off, 8u);
        if (v == reinterpret_cast<u64>(m.bus)) inv.fields.push_back({off, v, "memory_"});
        else if (v == reinterpret_cast<u64>(m.code)) inv.fields.push_back({off, v, "recompiled_"});
        else if (v == window) window_refs.emplace_back(off, v);
        else if (v == null_window) null_refs.emplace_back(off, v);
    }
    // The Cpu holds fm_window_, fm_null_window_ and, declared after them, fm_base_, a second reference to the
    // window while MSR[DR] = 1 and MSR[PR] = 0 and to the null window otherwise.
    const bool translated = (m.cpu->msr() & (kMsrDR | kMsrPR)) == kMsrDR;
    const bool pattern = window != 0u && null_window != 0u && window_refs.size() == (translated ? 2u : 1u) &&
                         null_refs.size() == (translated ? 1u : 2u);
    if (pattern) {
        inv.host_pages = window;
        inv.null_window = null_window;
        inv.fields.push_back({window_refs[0].first, window_refs[0].second, "fm_window_"});
        inv.fields.push_back({null_refs[0].first, null_refs[0].second, "fm_null_window_"});
        inv.active_pages_offset = translated ? window_refs[1].first : null_refs[1].first;
    } else {
        rep.fail("could not locate fm_window_ / fm_null_window_ / fm_base_ in the Cpu object (found " +
                 std::to_string(window_refs.size()) + " window and " + std::to_string(null_refs.size()) +
                 " null-window references)");
    }
    const auto count = [&](const char* what) {
        return std::count_if(inv.fields.begin(), inv.fields.end(), [&](const PointerField& f) { return std::strcmp(f.what, what) == 0; });
    };
    if (count("memory_") != 1 || count("recompiled_") != 1)
        rep.fail("Cpu pointer fields memory_/recompiled_ not located exactly once");
    inv.hle_slots_bytes = expected_hle_slots(inv.hooks);
    std::size_t found = 0;
    for (std::size_t off = 0; off + inv.hle_slots_bytes.size() <= size; off += 4u) {
        if (std::memcmp(object + off, inv.hle_slots_bytes.data(), inv.hle_slots_bytes.size()) == 0) {
            if (found == 0) inv.hle_slots_offset = off;
            ++found;
        }
    }
    if (found != 1) rep.fail("HLE slot table pattern found " + std::to_string(found) + " times in the Cpu object");
    inv.table_hash = table_hash(*m.code);
    inv.table_bytes = m.code->table_bytes();
    inv.table_low = m.code->low();
    inv.table_high = m.code->high();
}

// FM: committed pages whose window bytes are NOT the region's host bytes (0 when every page aliases).
std::size_t window_alias_failures(Machine& m, const Invariants& inv) {
    auto* const window = reinterpret_cast<volatile u8*>(inv.host_pages);
    std::size_t bad = 0;
    for (const auto& region : m.bus->regions()) {
        for (u32 offset = 0; offset < region.committed; offset += Cpu::kHostPageSize) {
            u8* const host = region.host + offset;
            volatile u8* const through = window + region.base + offset;
            const u8 saved = *host;
            *host = static_cast<u8>(saved ^ 0xA5u);
            const bool same = *through == static_cast<u8>(saved ^ 0xA5u);
            *host = saved;
            if (!same) ++bad;
        }
    }
    return bad;
}

void check_invariants(Machine& m, Invariants& inv, Report& rep, const std::string& when) {
    ++rep.checks;
    const std::size_t violations = f1c::guard::check_all("check");
    if (violations != 0) rep.fail(when + ": guard allocator found " + std::to_string(violations) + " corrupted canaries");
    const auto* object = reinterpret_cast<const u8*>(m.cpu);
    for (const PointerField& f : inv.fields) {
        u64 v = 0;
        std::memcpy(&v, object + f.offset, 8u);
        if (v != f.value) rep.fail(when + ": Cpu field " + f.what + " changed");
    }
    if (inv.active_pages_offset != ~std::size_t{0}) {
        // C2 / FM: the window the inline accessors address must be the flat window exactly in translated
        // supervisor mode and the null window in every other mode.
        u64 v = 0;
        std::memcpy(&v, object + inv.active_pages_offset, 8u);
        const bool translated_now = (m.cpu->msr() & (kMsrDR | kMsrPR)) == kMsrDR;
        const bool ok = translated_now ? v == inv.host_pages : v == inv.null_window;
        if (!ok) rep.fail(when + ": Cpu::fm_base_ does not match the MSR translation mode");
    }
    if (inv.hle_slots_offset != ~std::size_t{0} &&
        std::memcmp(object + inv.hle_slots_offset, inv.hle_slots_bytes.data(), inv.hle_slots_bytes.size()) != 0)
        rep.fail(when + ": HLE slot table bytes changed");
    const auto& entries = m.cpu->hle_entries();
    if (entries.size() != inv.hooks.size()) rep.fail(when + ": HLE hook count changed");
    for (std::size_t i = 0; i < entries.size() && i < inv.hooks.size(); ++i) {
        if (entries[i].address != inv.hooks[i].first || entries[i].name != inv.hooks[i].second)
            rep.fail(when + ": HLE hook entry changed");
    }
    if (inv.host_pages != 0) {
        // FM: every committed page of every region is the same memory through the window as through the
        // region's host view (a marker written through one is read through the other, then restored).
        if (const std::size_t bad = window_alias_failures(m, inv))
            rep.fail(when + ": " + std::to_string(bad) + " committed pages do not alias between the window and the host view");
    }
    if (m.code->low() != inv.table_low || m.code->high() != inv.table_high || m.code->table_bytes() != inv.table_bytes ||
        table_hash(*m.code) != inv.table_hash)
        rep.fail(when + ": recompiled lookup table changed");
    for (std::size_t i = 0; i < recomp::kFunctionCount; ++i) {
        if (m.code->lookup(recomp::kFunctions[i].address) != recomp::kFunctions[i].fn) {
            rep.fail(when + ": lookup of a declared entry no longer returns its function");
            break;
        }
    }
    if (!f1c::guard_page_below_limit()) rep.fail(when + ": host stack guard page missing below the committed stack");
    if (g_non_native_ppc_executions.load() != 0u) rep.fail(when + ": non-native PPC executions");
}

void load_code(Machine& m) {
    for (const f1c::CodeRow& row : f1c::kRows) {
        for (std::size_t i = 0; i < row.count; ++i) m.bus->write32(row.address + static_cast<u32>(4u * i), row.words[i]);
    }
}

// The OS's exception entry as __OSExceptionInit / __OSInitSystemCall / OSSetCurrentContext leave it. The native
// exception entry (gekko/os_exception_entry.cpp) performs the vector's work itself, but only after it has checked
// that the guest built the whole 38-word vector page (os_exception_page_word: the __OSExceptionVector template with
// `li r3, <index>` at +0x68, PPCSync's head over the first seven words for the system call). The F1c image carries
// none of those bytes, so the harness writes the same pages the guest's OS init would.
void install_os_entry(Machine& m) {
    gekko::write_os_exception_vector(*m.bus, 0x80000500u, 4u);  // external
    gekko::write_os_exception_vector(*m.bus, 0x80000900u, 8u);  // decrementer
    for (u32 word = 0; word < gekko::kOsVectorWords; ++word)
        m.bus->write32(0x80000C00u + 4u * word, gekko::os_exception_page_word(word, 9u));  // system call
    m.bus->write32(0x80003000u + 4u * 4u, f1c::kExternalHandler);
    m.bus->write32(0x80003000u + 4u * 8u, f1c::kDecrementerHandler);
    m.bus->write32(0x800000D4u, f1c::kOsContext);
}

void write_table(Machine& m, const u32* table, std::size_t entries) {
    for (std::size_t k = 0; k < entries; ++k) m.bus->write32(f1c::kDataBase + 4u * static_cast<u32>(k), table[k]);
}

// The program's end boundary: its final blr reached the untranslated sentinel, where the production dispatcher
// stops, with the original stack pointer.
bool stopped_at_sentinel(const Cpu& c) {
    char expected[96];
    std::snprintf(expected, sizeof(expected), "native execution cannot continue at 0x%08X", f1c::kSentinel);
    return c.stop().reason == StopReason::ExecutionFault && c.stop().detail.rfind(expected, 0) == 0 &&
           c.pc == f1c::kSentinel && c.gpr[1] == f1c::kStackTop;
}

// ---- phase A: shapes through the dispatcher, repeated ---------------------------------------------------------
void phase_shapes(Machine& m, Invariants& inv, Report& rep, const Options& o) {
    Cpu& c = *m.cpu;
    std::map<std::string, std::vector<std::uintptr_t>> watermarks;
    std::map<std::string, std::vector<bool>> sampled;
    std::map<std::string, bool> must_sample;
    u64 runs = 0;
    for (u64 rep_index = 0; rep_index < o.shape_reps; ++rep_index) {
        for (const f1c::Shape& s : f1c::kShapes) {
            if (s.hook != 0u && !m.cpu->has_hle(s.hook)) {
                // Installed once, before the invariants are captured (see run()).
                rep.fail(std::string("shape hook missing for ") + s.name);
            }
            for (u32 i = 0; i < 32u; ++i) c.gpr[i] = 0u;
            c.gpr[1] = f1c::kStackTop;
            c.gpr[5] = static_cast<u32>(o.shape_iterations);
            c.gpr[6] = f1c::kDataBase;
            write_table(m, s.table, 8u);
            for (u32 k = 8u; k < 16u; ++k) m.bus->write32(f1c::kDataBase + 4u * k, 0u);
            c.lr = f1c::kSentinel;
            if ((rep_index & 1u) != 0u) c.set_msr(c.msr() | (kMsrEE)); else c.set_msr(c.msr() & (~kMsrEE));
            c.set_decrementer(0x7FFFFFFFu);
            c.clear_stop();
            c.set_entry(s.entry);
            m.lowest = ~std::uintptr_t{0};
            m.samples = 0;
            m.mode = Machine::Mode::Sample;
            c.run();
            m.mode = Machine::Mode::Off;
            ++runs;
            if (!stopped_at_sentinel(c) || c.gpr[3] != static_cast<u32>(o.shape_iterations))
                rep.fail(std::string("phase A: shape ") + s.name + " did not complete (" + c.stop().detail.substr(0, 80) + ")");
            sampled[s.name].push_back(m.samples != 0u);
            watermarks[s.name].push_back(m.samples != 0u ? inv.depth_base - m.lowest : 0u);
            must_sample[s.name] = makes_host_call_every_iteration(s);
            check_invariants(m, inv, rep, std::string("phase A after ") + s.name);
        }
    }
    std::vector<std::string> unsampled;
    bool any_sampled = false;
    for (const auto& [name, marks] : watermarks) {
        const std::vector<bool>& seen = sampled[name];
        const bool all = std::all_of(seen.begin(), seen.end(), [](bool b) { return b; });
        const bool none = std::none_of(seen.begin(), seen.end(), [](bool b) { return b; });
        any_sampled = any_sampled || !none;
        if (none) {
            // The loop entered no translated entry thunk: no host call, so no host frame to observe. Not a drift
            // result - unless gen_f1c.py built the shape with a host call in every iteration.
            if (must_sample[name])
                rep.fail("phase A: shape " + name + " reached no stack probe although its loop makes a host call "
                         "every iteration (the probe set or the call path regressed)");
            else
                unsampled.push_back(name);
        } else if (!all) {
            rep.fail("phase A: shape " + name + " reached a stack probe in some repetitions only");
        } else if (std::adjacent_find(marks.begin(), marks.end(), std::not_equal_to<>()) != marks.end()) {
            rep.fail("phase A: host stack watermark of " + name + " differs between repetitions");
        }
    }
    if (!watermarks.empty() && !any_sampled) rep.fail("phase A: no shape reached a stack probe (the probes never ran)");
    char b[256];
    std::snprintf(b, sizeof(b), "{\"phase\":\"A\",\"runs\":%llu,\"iterations\":%llu,\"shapes\":%zu,\"reps\":%llu,\"watermarks\":{",
                  static_cast<unsigned long long>(runs), static_cast<unsigned long long>(runs * o.shape_iterations),
                  watermarks.size(), static_cast<unsigned long long>(o.shape_reps));
    std::string j = b;
    bool first = true;
    for (const auto& [name, marks] : watermarks) {
        if (!first) j += ',';
        first = false;
        j += quote(name) + ":[";
        const std::vector<bool>& seen = sampled[name];
        for (std::size_t i = 0; i < marks.size(); ++i) j += (i ? "," : "") + (seen[i] ? std::to_string(marks[i]) : std::string("null"));
        j += "]";
    }
    j += "},\"unsampled\":[";
    for (std::size_t i = 0; i < unsampled.size(); ++i) j += (i ? "," : "") + quote(unsampled[i]);
    rep.phases.push_back(j + "]}");
}

// ---- phase B1: longrun -----------------------------------------------------------------------------------------
struct Ledger {
    u64 services = 0;
    u64 asserts = 0;
    u64 acks = 0;
    bool asserted = false;
    u32 ack_r23 = 0;
    u64 next_check = kCheckEvery;
    u64 checks = 0;
    u64 dec_delivered = 0;
    u64 dec_bad_advances = 0;   // deliveries whose deadline was not DEC_RELOAD + 1 ticks after the last one
    u64 clock_advanced = 0;     // game ticks the device moved the clock, in total
    u64 poll_completions = 0;
};

// FM: the committed pages the window serves (each proven to alias the region's host bytes).
std::size_t live_host_pages(Machine& m, const Invariants& inv) {
    if (inv.host_pages == 0) return 0;
    std::size_t live = 0;
    for (const auto& region : m.bus->regions()) live += region.committed >> Cpu::kHostPageShift;
    return live - window_alias_failures(m, inv);
}

// The pages every region has committed: what the page table must serve.
std::size_t committed_pages(const GuestMemory& memory) {
    std::size_t pages = 0;
    for (const auto& region : memory.regions()) pages += region.committed >> Cpu::kHostPageShift;
    return pages;
}

void phase_longrun(Machine& m, Invariants& inv, Report& rep, const Options& o) {
    Cpu& c = *m.cpu;
    const char* name = "B1";
    const u64 outer = o.outer;
    const f1c::CanaryProgram& p = f1c::kCanary[0];
    CpuDiagCounters& diag = cpu_diag();
    for (u32 i = 0; i < 32u; ++i) c.gpr[i] = 0u;
    c.gpr[1] = f1c::kStackTop;
    c.gpr[5] = static_cast<u32>(outer);
    c.gpr[6] = f1c::kDataBase;
    write_table(m, p.table, 16u);
    m.bus->write32(f1c::kDataBase + f1c::k_longrun_poll_word, 0u);
    c.lr = f1c::kSentinel;
    c.set_msr(c.msr() & (~kMsrEE));
    c.set_decrementer(f1c::k_longrun_dec_reload);
    c.set_external_interrupt(false);
    c.clear_stop();
    c.set_entry(p.entry);
    const u64 exits0 = c.interpreter_exits;
    const u64 calls0 = c.recompiled_calls;
    u64 fallback0 = 0;
    for (std::size_t r = 0; r < static_cast<std::size_t>(FallbackReason::Count); ++r) fallback0 += c.fallback_total(static_cast<FallbackReason>(r));
    const u64 ext0 = diag.exceptions_by_vector[5], dec0 = diag.exceptions_by_vector[9], sc0 = diag.exceptions_by_vector[12];
    const u64 clock0 = c.game_clock().now();
    const u64 tb0 = c.time_base();
    Ledger L;
    const u32 poll_word = f1c::kDataBase + f1c::k_longrun_poll_word;
    const std::string tag = std::string("phase ") + name + ": ";
    m.service = [&](Cpu& cpu) {
        ++L.services;
        // The external line is a level the device drops once the guest's external handler counted it.
        if (L.asserted && cpu.gpr[23] != L.ack_r23) {
            cpu.set_external_interrupt(false);
            L.asserted = false;
            ++L.acks;
        }
        if (!L.asserted && L.services % o.irq_every == 0u) {
            L.ack_r23 = cpu.gpr[23];
            cpu.set_external_interrupt(true);
            L.asserted = true;
            ++L.asserts;
        }
        // The decrementer edge, delivered as the scheduler delivers GameEvent::Decrementer at a wait point: the
        // clock moves to the deadline the guest armed (its decrementer handler's mtdec reload) and the edge
        // latches.
        // One edge at a time, so every delivered edge is accounted for as taken or still latched.
        if (L.services % o.dec_every == 0u && !cpu.decrementer_pending()) {
            GameClock& clock = cpu.game_clock();
            const u64 now = clock.now();
            const u64 deadline = clock.deadline(GameEvent::Decrementer);
            if (deadline == kNoDeadline || deadline <= now ||
                deadline - now != static_cast<u64>(f1c::k_longrun_dec_reload) + 1u) {
                ++L.dec_bad_advances;
            }
            if (deadline != kNoDeadline && deadline > now) {
                L.clock_advanced += deadline - now;
                clock.set_now(deadline);
            }
            cpu.latch_decrementer();
            ++L.dec_delivered;
        }
        if (L.services >= L.next_check) {
            L.next_check += kCheckEvery;
            ++L.checks;
            check_invariants(m, inv, rep, tag + "periodic");
        }
    };
    m.poll_device = [&]() {
        m.bus->write32(poll_word, 1u);
        ++L.poll_completions;
    };
    m.mode = Machine::Mode::Longrun;
    const double t0 = f1c::now_ms();
    c.run();
    const double wall = f1c::now_ms() - t0;
    m.mode = Machine::Mode::Off;
    m.service = nullptr;
    m.poll_device = nullptr;
    c.set_external_interrupt(false);
    if (!stopped_at_sentinel(c)) rep.fail(tag + "did not return to the sentinel with r1 restored: " + c.stop().detail.substr(0, 120));
    // Native-time accounting (replaces time_base == total_instructions / 12 and recompiled == total): guest code
    // never moves the game clock, so it moved by exactly what the device's deliveries advanced, and the time base
    // and the decrementer are views of it.
    const u64 clock1 = c.game_clock().now();
    if (clock1 - clock0 != L.clock_advanced)
        rep.fail(tag + "the game clock moved " + std::to_string(clock1 - clock0) + " ticks, the device advanced it " +
                 std::to_string(L.clock_advanced) + " (guest code moved the clock)");
    if (c.time_base() - tb0 != clock1 - clock0) rep.fail(tag + "time base delta != game clock delta");
    if (L.dec_bad_advances != 0u)
        rep.fail(tag + std::to_string(L.dec_bad_advances) + " decrementer deadlines were not DEC_RELOAD + 1 ticks after "
                 "the guest's reload");
    const u32 expected_dec = c.decrementer_pending() ? 0xFFFFFFFFu : static_cast<u32>(f1c::k_longrun_dec_reload);
    if (c.decrementer() != expected_dec) rep.fail(tag + "DEC is not the reload (or -1 with an edge latched) at the end");
    // The device ledger (replaces hardware_ticks == services and the per-service `retired` ledger): every guest
    // call of the leaf passed through its entry thunk's try_hle exactly once, and every idle loop was completed.
    const u64 a = f1c::k_longrun_inner_a, bb = f1c::k_longrun_inner_b;
    if (L.services != 2u * outer * a) rep.fail(tag + "device services " + std::to_string(L.services) + " != leaf calls 2*outer*INNER_A");
    if (L.poll_completions != outer) rep.fail(tag + "idle-loop device completions != outer iterations");
    u64 fallback = 0;
    for (std::size_t r = 0; r < static_cast<std::size_t>(FallbackReason::Count); ++r) fallback += c.fallback_total(static_cast<FallbackReason>(r));
    if (fallback - fallback0 != c.interpreter_exits - exits0) rep.fail(tag + "fallback totals != interpreter_exits");
    const u64 ext = diag.exceptions_by_vector[5] - ext0, dec = diag.exceptions_by_vector[9] - dec0, sc = diag.exceptions_by_vector[12] - sc0;
    if (c.gpr[23] != static_cast<u32>(ext)) rep.fail(tag + "guest external-handler entries (r23) != raised external interrupts");
    if (c.gpr[24] != static_cast<u32>(dec)) rep.fail(tag + "guest decrementer-handler entries (r24) != raised decrementer exceptions");
    if (c.gpr[28] != static_cast<u32>(sc) || sc != outer) rep.fail(tag + "sc executions (r28) != the sc census != outer iterations");
    if (L.asserts == 0 || ext < L.asserts - (L.asserted ? 1u : 0u))
        rep.fail(tag + "external interrupts asserted but not all taken");
    if (L.dec_delivered == 0u) rep.fail(tag + "no decrementer edge was delivered");
    if (L.dec_delivered != dec + (c.decrementer_pending() ? 1u : 0u))
        rep.fail(tag + "delivered decrementer edges != decrementer-handler entries + the edge still latched");
    u64 r27 = 0;
    for (u64 k = 0; k < outer; ++k) {
        const u64 base = k * (a + bb);
        for (u64 i = 1; i <= a; ++i) r27 += ((base + i) & 3u) + 1u;
    }
    if (c.gpr[4] != outer || c.gpr[25] != outer || c.gpr[26] != outer) rep.fail(tag + "outer loop counters wrong");
    if (c.gpr[3] != static_cast<u32>(outer * (a + bb))) rep.fail(tag + "r3 != outer*(INNER_A+INNER_B)");
    if (c.gpr[7] != static_cast<u32>(2u * outer * a)) rep.fail(tag + "leaf call count r7 wrong");
    if (c.gpr[27] != static_cast<u32>(r27)) rep.fail(tag + "jump-table case sum r27 wrong");
    if (c.gpr[29] != 0u) rep.fail(tag + "store/load across the host page boundary mismatched (r29)");
    if ((c.msr() & kMsrEE) != 0u) rep.fail(tag + "MSR[EE] left set");
    if (live_host_pages(m, inv) != committed_pages(*m.bus))
        rep.fail(tag + "the flat window does not serve every committed page");
    check_invariants(m, inv, rep, tag + "end");
    char b[1024];
    std::snprintf(b, sizeof(b),
                  "{\"phase\":\"%s\",\"outer\":%llu,\"services\":%llu,\"poll_completions\":%llu,\"irq_every\":%llu,"
                  "\"dec_every\":%llu,\"ext_interrupts\":%llu,\"asserts\":%llu,\"acks\":%llu,\"dec_delivered\":%llu,"
                  "\"dec_exceptions\":%llu,\"sc\":%llu,\"periodic_checks\":%llu,\"interpreter_exits\":%llu,"
                  "\"yields\":%llu,\"native_calls\":%llu,\"clock_start\":%llu,\"clock_end\":%llu,\"time_base\":%llu,"
                  "\"dec\":\"0x%08X\",\"wall_ms\":%.0f}",
                  name, static_cast<unsigned long long>(outer), static_cast<unsigned long long>(L.services),
                  static_cast<unsigned long long>(L.poll_completions), static_cast<unsigned long long>(o.irq_every),
                  static_cast<unsigned long long>(o.dec_every), static_cast<unsigned long long>(ext),
                  static_cast<unsigned long long>(L.asserts), static_cast<unsigned long long>(L.acks),
                  static_cast<unsigned long long>(L.dec_delivered), static_cast<unsigned long long>(dec),
                  static_cast<unsigned long long>(sc), static_cast<unsigned long long>(L.checks),
                  static_cast<unsigned long long>(c.interpreter_exits - exits0),
                  static_cast<unsigned long long>(c.fallback_total(FallbackReason::Yield)),
                  static_cast<unsigned long long>(c.recompiled_calls - calls0), static_cast<unsigned long long>(clock0),
                  static_cast<unsigned long long>(clock1), static_cast<unsigned long long>(c.time_base()), c.decrementer(),
                  wall);
    rep.phases.push_back(b);
}

struct Job {
    Options o;
    Report rep;
    std::string guard_before;
};

void run(void* arg) {
    Job& job = *static_cast<Job*>(arg);
    const Options& o = job.o;
    Report& rep = job.rep;
    if (f1c::guard::check_all("start") != 0) rep.fail("guard allocator violations before the test started");
    Machine m;
    m.code = new RecompiledCode();
    m.code->install(recomp::kFunctions, recomp::kFunctionCount);
    m.code->install_ranges(recomp::kRanges, recomp::kRangeCount);
    m.bus = new GuestMemory();
    m.cpu = new Cpu(*m.bus);
    load_code(m);
    install_os_entry(m);
    m.cpu->reset();
    m.cpu->attach_recompiled(m.code);
    Invariants inv;
    inv.depth_base = probe_address();
    // Hooks, installed once (the slot table must never change afterwards): the shapes' HLE-served callee, the leaf
    // probe (phase A stack sample / B1 device service), the shapes' record-head stack probes and the idle-loop
    // device, which is also the dispatcher-top probe (the dispatcher resumes there after every sc). Every probe
    // declines, so the translated code it sits in runs unchanged.
    Invariants* invp = &inv;
    Machine* mp = &m;
    m.cpu->install_hle(f1c::kHleCallee, [mp](Cpu& c) {
        if (mp->mode == Machine::Mode::Sample) sample_stack(*mp);
        c.gpr[7] += 1u;
        return true;
    }, "f1c served callee");
    m.cpu->install_hle(f1c::kLeaf, [mp](Cpu& c) {
        if (mp->mode == Machine::Mode::Sample) sample_stack(*mp);
        else if (mp->mode == Machine::Mode::Longrun && mp->service) mp->service(c);
        return false;
    }, "f1c leaf probe / device service");
    std::set<u32> heads;
    for (const f1c::Shape& s : f1c::kShapes) {
        for (const u32 address : shape_record_heads(s)) heads.insert(address);
    }
    for (const u32 address : heads) {
        m.cpu->install_hle(address, [mp](Cpu&) {
            if (mp->mode == Machine::Mode::Sample) sample_stack(*mp);
            return false;
        }, "f1c stack probe");
    }
    m.cpu->install_hle(f1c::k_longrun_label_poll, [mp, invp](Cpu&) {
        if (mp->mode == Machine::Mode::Longrun && mp->poll_device) {
            const std::uintptr_t depth = invp->depth_base - probe_address();
            invp->top_min = std::min(invp->top_min, depth);
            invp->top_max = std::max(invp->top_max, depth);
            ++invp->top_samples;
            mp->poll_device();
        }
        return false;
    }, "f1c idle-loop device / dispatcher-top probe");
    for (const auto& e : m.cpu->hle_entries()) inv.hooks.emplace_back(e.address, e.name);
    capture_invariants(m, inv, rep);
    check_invariants(m, inv, rep, "initial");
    if (o.phases.count("A")) phase_shapes(m, inv, rep, o);
    if (o.phases.count("B1")) phase_longrun(m, inv, rep, o);
    if (inv.top_samples != 0 && inv.top_min != inv.top_max) {
        rep.fail("dispatcher-top host stack depth drifted: min " + std::to_string(inv.top_min) + " max " +
                 std::to_string(inv.top_max) + " over " + std::to_string(inv.top_samples) + " dispatches after sc");
    }
    if (o.phases.count("B1") && inv.top_samples == 0) rep.fail("the dispatcher-top probe never ran (no sc dispatch)");
    char b[256];
    std::snprintf(b, sizeof(b), "{\"phase\":\"dispatcher_top\",\"samples\":%llu,\"min\":%llu,\"max\":%llu}",
                  static_cast<unsigned long long>(inv.top_samples), static_cast<unsigned long long>(inv.top_samples ? inv.top_min : 0u),
                  static_cast<unsigned long long>(inv.top_max));
    rep.phases.push_back(b);
    std::string fields = "{\"phase\":\"located\",\"cpu_size\":" + std::to_string(sizeof(Cpu)) + ",\"bus_size\":" +
                         std::to_string(sizeof(GuestMemory)) + ",\"hle_slots_offset\":" + std::to_string(inv.hle_slots_offset) +
                         ",\"hooks\":" + std::to_string(inv.hooks.size()) + ",\"fields\":[";
    for (std::size_t i = 0; i < inv.fields.size(); ++i)
        fields += (i ? ",{" : "{") + std::string("\"what\":") + quote(inv.fields[i].what) + ",\"offset\":" + std::to_string(inv.fields[i].offset) + "}";
    rep.phases.push_back(fields + "]}");
    char t[256];
    std::snprintf(t, sizeof(t), "{\"phase\":\"final\",\"native_calls\":%llu,\"interpreter_exits\":%llu,\"hle_slow_calls\":%llu,"
                                "\"game_clock\":%llu}",
                  static_cast<unsigned long long>(m.cpu->recompiled_calls),
                  static_cast<unsigned long long>(m.cpu->interpreter_exits),
                  static_cast<unsigned long long>(m.cpu->hle_slow_calls()),
                  static_cast<unsigned long long>(m.cpu->game_clock().now()));
    delete m.cpu;
    delete m.bus;
    delete m.code;
    if (f1c::guard::check_all("end") != 0) rep.fail("guard allocator violations at teardown");
    rep.phases.push_back(t);
}

}  // namespace

int main(int argc, char** argv) {
    Job job;
    Options& o = job.o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--out") o.out = value();
        else if (a == "--outer") o.outer = std::strtoull(value().c_str(), nullptr, 0);
        else if (a == "--shape-reps") o.shape_reps = std::strtoull(value().c_str(), nullptr, 0);
        else if (a == "--shape-iterations") o.shape_iterations = std::strtoull(value().c_str(), nullptr, 0);
        else if (a == "--irq-every") o.irq_every = std::strtoull(value().c_str(), nullptr, 0);
        else if (a == "--dec-every") o.dec_every = std::strtoull(value().c_str(), nullptr, 0);
        else if (a == "--phases") {
            o.phases.clear();
            std::string list = value();
            std::size_t start = 0;
            while (start <= list.size()) {
                const std::size_t comma = list.find(',', start);
                const std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!item.empty()) o.phases.insert(item);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        } else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    for (const std::string& phase : o.phases) {
        if (phase == "B2" || phase == "B2x" || phase == "B3") {
            std::fprintf(stderr, "phase %s is retired (native time): it measured the instruction counter, the "
                                 "instruction-derived timers and coalesced hardware-service charges, none of which "
                                 "exists any more (see the header of f1c_canary_driver.cpp)\n", phase.c_str());
            return 2;
        }
        if (phase != "A" && phase != "B1") { std::fprintf(stderr, "unknown phase %s\n", phase.c_str()); return 2; }
    }
    if (o.out.empty() || o.irq_every == 0 || o.dec_every == 0 || o.outer == 0 || o.outer > 0xFFFFFFFFull) {
        std::fprintf(stderr, "usage: f1c_canary_driver --out report.json [--outer N] [--shape-reps N] "
                             "[--shape-iterations N] [--irq-every N] [--dec-every N] [--phases A,B1]\n");
        return 2;
    }
    const unsigned long code = f1c::run_on_thread(std::size_t{1} << 30, &run, &job);
    if (code != 0) {
        char b[96];
        std::snprintf(b, sizeof(b), "guest thread died with host exception 0x%08lX", code);
        job.rep.fail(b);
    }
    std::string j = "{\"schema\":\"gekko-f1c-canary-report/2\",\"thread_exit\":" + hex64(code) + ",\"checks\":" +
                    std::to_string(job.rep.checks) + ",\"guard\":" + f1c::guard::summary_json() + ",\"phases\":[";
    for (std::size_t i = 0; i < job.rep.phases.size(); ++i) j += (i ? "," : "") + job.rep.phases[i];
    j += "],\"failures\":[";
    for (std::size_t i = 0; i < job.rep.failures.size(); ++i) j += (i ? "," : "") + quote(job.rep.failures[i]);
    j += "],\"passed\":" + std::string(job.rep.failures.empty() ? "true" : "false") + "}";
    {
        std::error_code ignored;
        const std::filesystem::path parent = std::filesystem::path(o.out).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ignored);
    }
    std::ofstream out(o.out, std::ios::binary);
    out << j << "\n";
    std::printf("%s\n", j.c_str());
    return job.rep.failures.empty() ? 0 : 1;
}
