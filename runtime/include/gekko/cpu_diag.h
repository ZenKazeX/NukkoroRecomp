#pragma once

// Conformance diagnostics for the Gekko CPU model.
//
// Deliberately NOT in cpu.h. Every one of the 64 generated recomp shards
// includes cpu.h through recomp.h, so a field added there costs a ~25 minute
// full rebuild. These counters answer questions about behaviour the generated
// code never touches - exception delivery and the FPR snapshot - so they live
// in a header only cpu.cpp and shadow_main.cpp include, and cost a relink.
//
// Why they exist. The 2026-09-03 conformance audit against the IBM Gekko User's
// Manual (docs/reference/ppc/gekko_user_manual.txt) raised three questions that
// the four standing gates structurally cannot answer, because three of the four
// are hashes of this port's own output and can only prove "nothing changed":
//
//   A-3  PAGE 164 / 166 / 475: MSR[EE] = 0 must DELAY a decrementer exception
//        condition. cpu.cpp used to fold the EE test into the edge detection,
//        destroying an edge that landed with EE clear; fixed 2026-09-13 (audit
//        dispatch-exceptions-1). `dec_edge_suppressed_ee_clear` now counts the
//        edges that were delayed rather than lost.
//
//   B-1  restore_interrupt_fpr copies 32 FPRs back on rfi. Its stated
//        justification - that FpUnavailable "is declared and never raised" - is
//        stale. Does the restore branch ever actually run?
//
//   plus a per-vector exception census, because the published counters
//   (sc 6106, rfi 14802) are not split by vector and cannot show which
//   handlers run at all.
//
// Counting only. Nothing here participates in guest state, retirement
// accounting, savestates or --verify-recomp, so every hash must be unchanged
// when this lands. That invariance IS the acceptance test.

#include <array>
#include <atomic>
#include <unordered_map>

#include "gekko/types.h"

namespace gekko {

struct CpuDiagCounters {
    // Indexed by (vector >> 8), so 0x00900 (Decrementer) is slot 9. 16 slots
    // covers every vector the enum defines (0x00100..0x00F00).
    static constexpr std::size_t kVectorSlots = 16;
    std::array<u64, kVectorSlots> exceptions_by_vector{};
    u64 exceptions_total = 0;

    // A-3, resolved by audit dispatch-exceptions-1 (2026-09-13): every edge is
    // latched and taken once MSR[EE] allows, so nothing is discarded any more.
    u64 dec_edge_total = 0;               // every 0 -> 1 transition observed
    u64 dec_edge_suppressed_ee_clear = 0; // ...of which arrived with EE = 0 (delayed)
    // Always 0 since that fix: the dispatcher no longer clears a latched
    // condition it cannot take. Kept so the census output keeps its fields.
    u64 dec_pending_dropped_ee_clear = 0;
    u64 dec_pending_taken = 0;            // 0x900 entries from the latch
    // Native interrupt deliveries entered (gekko/native_interrupts.h).
    u64 native_interrupts_entered = 0;
    u64 os_exception_entries = 0;  // OS vector work done natively (os_exception_entry.cpp)
    // Cpu::poll_point: progress back edges taken, watches started and fixed
    // points (waits). Bus::watch_aborts counts the watches real work ended.
    u64 poll_points = 0;
    u64 poll_watches = 0;
    u64 poll_fixed_points = 0;

    // B-1. Outcomes of restore_interrupt_fpr's final branch.
    u64 fpr_restore_performed = 0;  // 32 FPRs + FPSCR actually copied back
    u64 fpr_restore_skipped_context_switch = 0;  // SRR0 moved: snapshot dropped
    // Of the restores actually performed, how many wrote a value that differed
    // from what the handler left behind, and how many registers differed in
    // total. A restore that changes nothing is pure overhead; one that changes
    // something is load-bearing and the shadow cannot simply be deleted. This
    // decides whether the shadow copy is removable - measure before changing.
    u64 fpr_restore_changed = 0;
    u64 fpr_restore_changed_registers = 0;

    // ---- 2026-09-05 forensic counters: the dispatcher ----------------------
    //
    // Dispatcher accounting. `dispatch_iterations` counts every pass of the
    // Cpu::run loop; `native_calls_timed` the native entries it timed. Time is
    // steady_clock nanoseconds: inside native calls (everything the call tree
    // did, wait points and renderer waits included) versus the dispatcher's
    // own work between them (exception delivery, table lookups, HLE checks).
    bool dispatch_timing = false;
    u64 dispatch_iterations = 0;
    u64 native_calls_timed = 0;
    u64 native_normal_returns = 0;     // native call returned without leave_to_dispatcher
    u64 run_ns = 0;                    // wall inside Cpu::run
    u64 native_ns = 0;                 // wall inside native(*this) / range->at()
    u64 interrupt_raise_ns = 0;        // wall in raise_exception at the dispatcher

    // ---- 2026-09-05 INTERPRETER ERADICATION AUDIT --------------------------
    //
    // TOTAL_NON_NATIVE_PPC_EXECUTIONS. Incremented at the deepest common point
    // where a guest instruction's semantics are executed by anything other
    // than generated native code: the top of Cpu::execute(u32 instruction),
    // which every interpreted instruction passes through (step() -> execute()).
    // Not incremented for dispatcher iterations, HLE service or intrinsics -
    // those have their own ledgers below. Atomic so no later
    // threading change can make it lie.
    // (the counter itself is the process-wide atomic below, outside this
    // struct: --verify-recomp snapshots and restores the census fields a
    // native pass can advance, and a hard counter must not be rolled back with
    // them. VERIFY CONTRACT: a counter written from raise_exception,
    // restore_interrupt_fpr or anything else native code reaches must be
    // added to VerifyDiagSnapshot in cpu.cpp, or a discarded verification
    // pass counts it twice.)
    // Why the machine was interpreting at that moment (set by the dispatcher
    // right before it steps).
    //   1 HLE hook at pc blocks native dispatch   2 no native code and no range
    //   3 trap-first-instruction retry            4 no recompiled table at all
    //   5 --verify-recomp oracle replay (never stored as an origin; counted in
    //     verify_replay_executions)               0 unknown
    u8 interpreter_cause = 0;
    struct NonNativeOrigin {
        u32 pc = 0;
        u32 instruction = 0;
        u32 lr = 0;
        u8 cause = 0;
        u64 count = 0;
        u64 first_interpreted = 0;  // interpreter instruction count at first sight
        u64 first_ns = 0;
        u64 thread_hash = 0;
    };
    static constexpr std::size_t kNonNativeOrigins = 256;
    std::array<NonNativeOrigin, kNonNativeOrigins> non_native_origins{};
    u64 non_native_origins_overflow = 0;
    // Instructions executed by the --verify-recomp interpreter replay (cause
    // 5). A scalar, not origin slots: per-pc recording saturated the table and
    // cost a 256-entry scan per replayed instruction.
    u64 verify_replay_executions = 0;

    // Generated-function entry counts (--call-trace): the "actual host
    // execution count" of a native function, keyed by guest entry address.
};

// Process-wide, single Cpu per process. Not atomic: every increment site runs
// on the guest thread.
CpuDiagCounters& cpu_diag();
bool reference_execution_available() noexcept;

// TOTAL_NON_NATIVE_PPC_EXECUTIONS. Process-wide, atomic, never reset or
// restored. Incremented only at the top of Cpu::execute(u32).
extern std::atomic<u64> g_non_native_ppc_executions;

}  // namespace gekko
