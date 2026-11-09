// The native gameplay scheduler. Contract and rationale:
// include/gekko/gameplay_scheduler.h.

#include "gekko/gameplay_scheduler.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gameplay_traversal.h"
#include "gekko/cpu.h"
#include "gekko/cpu_diag.h"
#include "gekko/guest_memory.h"
#include "gekko/profiling.h"
#include "gekko/recomp.h"

namespace gekko {
namespace {

// The guest functions this class stands in front of, and the two it still
// calls. Every address is an entry point in the PAL DOL, confirmed against the
// original instruction words.
constexpr u32 kTraversal = 0x8004ECACu;    // the scheduler; replaced
constexpr u32 kContinuation = 0x8004ECB0u; // its second instruction; never reached
                                           // by the guest once the entry is served,
                                           // so it is free as the return address
                                           // every dispatched task comes back to
constexpr u32 kMarkChildren = 0x8004EECCu; // request removal of a whole subtree
constexpr u32 kProfilingCall = 0x8004EDF0u;// the game's own timing wrapper

// Task node layout (64 bytes) and the two vtable slots the scheduler uses.
constexpr u32 kFlags = 0x04u;
constexpr u32 kNextSibling = 0x0Cu;
constexpr u32 kFirstChild = 0x14u;
constexpr u32 kVtable = 0x18u;
constexpr u32 kVtableDestructor = 0x08u;
constexpr u32 kVtableExecute = 0x0Cu;

// Scheduler state, in the small data area at r13 = 0x805EC500.
constexpr u32 kRecursionDepth = 0x805EF270u;  // read only inside the traversal
constexpr u32 kProfilerArmed = 0x805EF274u;   // no writer exists in this image
constexpr u32 kPauseState = 0x805EF27Cu;      // 14 readers outside the scheduler

// The original's own frame: 32 bytes, the caller's link at +0x24 of it, the
// saved registers where the original saves them. Reproduced field for field,
// so the stack image the game's tasks run above is the one they would have run
// above. Nothing of this scheduler's own state is written into it: every word
// a callee may touch is a word a callee may touch. +0x04 in particular belongs
// to the callee - it is where each called function saves the link register,
// which is how the task destructor overwrote an identity word kept there - and
// +0x08 upward is the outgoing parameter area. The two slots the original
// itself keeps live across its calls (+0x0C..0x1F) are the only ones this
// frame uses, for exactly what the original uses them for.
constexpr u32 kFrameSize = 32u;
constexpr u32 kFrameSavedR29 = 0x0Cu;
constexpr u32 kFrameSavedF31 = 0x18u;
constexpr u32 kFrameSavedLink = 0x24u;

// A task object: word aligned, 64 bytes of the game's mapped memory.
bool task_address_ok(const GuestMemory& memory, u32 address) {
    return (address & 3u) == 0u && memory.mapped(GcAddr(address), 64u);
}

// ============================================================================
// STEP-0 MEASUREMENT, audit item R3 - DEFAULT OFF, OBSERVATIONAL ONLY.
//
// findings-2026-09-23/HARDWARE_ASSUMPTION_AUDIT.md R3. The original traversal
// touches the FPU at six guarded places (the legacy translation of
// fn_8004ECAC_body raises FP-unavailable with SRR0 exactly at each of them):
//
//   0x8004ECB8 stfd f31,0x18(r1)  prologue, every level      -> native begin()
//   0x8004ED24 fmr  f1,f31        after MarkChildren returns -> continuation
//   0x8004ED80 fmr  f1,f31        instrumented path (DEAD: 0x805EF274 has no writer)
//   0x8004ED94 fmr  f1,f31        before the execute bctrl   -> dispatch_execute
//   0x8004EDA8 fmr  f1,f31        after execute / paused skip -> continuation
//   0x8004EDD8 lfd  f31,0x18(r1)  epilogue, every level      -> leave_level / finish
//
// Native checks MSR[FP] only in begin(). The probe observes the other points,
// never repairs anything, and records what the lazy-FPU protocol would do with
// the values the scheduler wrote. It writes no guest state, retires nothing,
// takes no exception, calls no dispatcher and installs no hook. Every guest
// read goes through Cpu::read_guest_u32 / read_guest_u8 (a const BAT walk plus
// Bus::host_pointer: MEM1 / locked cache / EFB only, no MMIO; anything else
// reads 0). Enabled only by enable_fp_probe().
//
// OS words, each confirmed against the PAL DOL (tools/doldis.py):
//   OS_CURRENT_CONTEXT     0x800000D4  OSSetCurrentContext 80372B84 906400D4 stw r3,0xd4(r4);
//                                      vector stub 8037168C 808000D4 lwz r4,0xd4(0)
//   OS_CURRENT_FPU_CONTEXT 0x800000D8  OSSwitchFPUContext 80373120 80A300D8 lwz r5,0xd8(r3),
//                                      80373124 908300D8 stw r4,0xd8(r3); owner==current
//                                      takes 8037312C beq (no save, no load)
//   active thread head     0x800000DC  OSCreateThread 80378BF4 93E400DC stw r31,0xdc(r4)
//   current thread         0x800000E4  80378C54 83DF00E4 lwz r30,0xe4(r31)
//   handler slot 7         0x8000301C  stub 803716AC 80A53000 lwz r5,0x3000(r5); __OSContextInit
//                                      80373194 addi r4,r3,0x3100 / 80373198 li r3,7
//   OSContext f1  +0x98    __OSSaveFPUContext 80372A60 D8250098; __OSLoadFPUContext 803729D0 C8240098
//   OSContext f31 +0x188   80372AD8 DBE50188 (save); 80372A48 CBE40188 (load)
//   FPSAVED = bit 0 of u16 +0x1A2 (byte +0x1A3): 80372A50 A06501A2 / 80372A54 ori / 80372A58 sth;
//             the load skips everything when clear (8037292C..80372934 beq 0x80372a4c)
//   OSThread active-queue next +0x2FC  80378BFC 93E602FC; the OSContext is at thread +0
constexpr u32 kOsCurrentContext = 0x800000D4u;
constexpr u32 kOsFpuContext = 0x800000D8u;
constexpr u32 kOsActiveThreadHead = 0x800000DCu;
constexpr u32 kOsCurrentThread = 0x800000E4u;
constexpr u32 kOsFpHandlerSlot = 0x8000301Cu;
constexpr u32 kOsSwitchFpuContext = 0x80373100u;
constexpr u32 kCtxF1 = 0x98u;
constexpr u32 kCtxF31 = 0x188u;
constexpr u32 kCtxStateLo = 0x1A3u;  // bit 0 = OS_CONTEXT_STATE_FPSAVED
constexpr u32 kThreadActiveNext = 0x2FCu;
constexpr u32 kThreadWalkCap = 64u;
constexpr u32 kTraversalEnd = 0x8004EDF0u;  // one past 0x8004ECAC's last instruction
constexpr std::size_t kFpVectorSlot =
    static_cast<std::size_t>(Exception::FpUnavailable) >> 8;  // 0x800 -> slot 8

enum class FpSite : std::uint8_t {
    Begin,
    ContExecute,
    ContInstrumented,
    ContMark,
    ContDestroy,
    ContYield,
    Execute,
    ExecuteInstrumented,
    PausedSkip,
    LeaveNested,
    Finish,        // outermost traversal of the active stack
    FinishNested,  // a traversal a task started (manager pump) or a nested begin
    DispatchMark,
    DispatchDestroy,
    DispatchYield,
    Count,
};
constexpr std::size_t kFpSiteCount = static_cast<std::size_t>(FpSite::Count);

struct FpSiteInfo {
    const char* name;
    u32 legacy_srr0;  // the guarded FP instruction the original executes here; 0 = none
    bool detail;      // FP=0 events are classified by owner and logged
};
constexpr FpSiteInfo kFpSiteInfo[kFpSiteCount] = {
    {"Begin", 0x8004ECB8u, true},
    {"ContExecute", 0x8004EDA8u, true},
    {"ContInstrumented", 0x8004EDA8u, true},
    {"ContMark", 0x8004ED24u, true},
    {"ContDestroy", 0u, true},  // 8004ED5C b 0x8004edb8: no FP instruction
    {"ContYield", 0u, true},    // back-edge resume: no FP instruction
    {"Execute", 0x8004ED94u, true},
    {"ExecuteInstrumented", 0x8004ED80u, true},
    {"PausedSkip", 0x8004EDA8u, false},  // 8004ED70 beq 0x8004eda8
    {"LeaveNested", 0x8004EDD8u, false},
    {"Finish", 0x8004EDD8u, true},
    {"FinishNested", 0x8004EDD8u, true},
    {"DispatchMark", 0u, false},
    {"DispatchDestroy", 0u, false},
    {"DispatchYield", 0u, false},
};
constexpr u32 kLegacyFpGuards[6] = {0x8004ECB8u, 0x8004ED24u, 0x8004ED80u,
                                    0x8004ED94u, 0x8004EDA8u, 0x8004EDD8u};

// The dispatched steps a continuation can follow, in JSON order.
constexpr std::size_t kFpStepSlots = 5;
constexpr const char* kFpStepNames[kFpStepSlots] = {"Execute", "ExecuteInstrumented",
                                                    "MarkChildren", "Destroy", "Yield"};

int fp_step_slot(shadow::gameplay::Step step) noexcept {
    using shadow::gameplay::Step;
    switch (step) {
    case Step::Execute: return 0;
    case Step::ExecuteInstrumented: return 1;
    case Step::MarkChildren: return 2;
    case Step::Destroy: return 3;
    case Step::Yield: return 4;
    case Step::Suspended:
    case Step::Finished: break;
    }
    return -1;
}

constexpr FpSite kFpContinuationSite[kFpStepSlots] = {
    FpSite::ContExecute, FpSite::ContInstrumented, FpSite::ContMark, FpSite::ContDestroy,
    FpSite::ContYield};

// Host-heap only, one per Traversal; written only while the probe is enabled.
struct FpDispatchRecord {
    shadow::gameplay::Step step = shadow::gameplay::Step::Finished;
    bool valid = false;
    u32 node = 0;
    u32 msr = 0;
    u32 d4 = 0;
    u32 d8 = 0;
    u64 fp_exceptions = 0;  // cpu_diag FP-unavailable count at dispatch
};

void appendf(std::string& out, const char* format, ...) {
    char buffer[768];
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (written > 0) {
        out.append(buffer,
                   std::min(static_cast<std::size_t>(written), sizeof(buffer) - 1u));
    }
}

unsigned long long ull(u64 value) { return static_cast<unsigned long long>(value); }

class FpProbe {
public:
    FpProbe(Cpu& cpu, std::function<u64()> vi_frame)
        : cpu_(cpu),
          vi_frame_(std::move(vi_frame)),
          enabled_at_ticks_(cpu.game_clock().now()),
          fp_exceptions_at_enable_(fp_exceptions()),
          fpr_shadow_at_enable_(cpu.interrupt_fpr_shadow()) {}

    // advance(), before the switch: every step the walk hands to the guest.
    void note_dispatch(FpDispatchRecord& record, shadow::gameplay::Step step,
                       u32 node) noexcept {
        const int slot = fp_step_slot(step);
        if (slot < 0) return;
        const Sample s = sample();
        record.step = step;
        record.valid = true;
        record.node = node;
        record.msr = s.msr;
        record.d4 = s.d4;
        record.d8 = s.d8;
        record.fp_exceptions = s.fp_exc;
        ++dispatches_[static_cast<std::size_t>(slot)];
        // Execute is observed at its own site (dispatch_execute), with the
        // registers it is about to write.
        switch (step) {
        case shadow::gameplay::Step::MarkChildren: account(FpSite::DispatchMark, s); break;
        case shadow::gameplay::Step::Destroy: account(FpSite::DispatchDestroy, s); break;
        case shadow::gameplay::Step::Yield: account(FpSite::DispatchYield, s); break;
        default: break;
        }
    }

    // begin(), immediately before its own MSR[FP] test.
    void on_begin(u32 parent, u64 traversal_id) noexcept {
        ++begins_;
        const Sample s = sample();
        const bool started = account(FpSite::Begin, s);
        if (s.fp_on) return;
        // A real trap in both arms: the handler resumes at 0x8004ECB8 inside
        // the translated guest body, which runs this whole call (and raises its
        // own guards, visible only to the r13 exception histogram).
        ++begin_fp_off_;
        const Owner o = classify(s);
        note_owner(FpSite::Begin, s, o);
        Event e = base_event(FpSite::Begin, s, o, traversal_id);
        e.node = parent;
        record(e, started, false);
    }

    // dispatch_execute(), between "gpr[3] = node" and the unguarded f1 write.
    void on_execute(u32 node, bool instrumented, u64 traversal_id) noexcept {
        const FpSite site = instrumented ? FpSite::ExecuteInstrumented : FpSite::Execute;
        if (instrumented) {
            ++instrumented_;
        } else {
            ++executes_;
        }
        const Sample s = sample();
        const bool started = account(site, s);
        if (s.fp_on) return;
        const Owner o = classify(s);
        note_owner(site, s, o);
        const u64 live_f1 = cpu_.fpr[1].bits[0];
        const u64 live_f31 = cpu_.fpr[31].bits[0];
        u32 entry = kProfilingCall;
        if (!instrumented) {
            const u32 vtable = cpu_.read_guest_u32(node + kVtable);
            entry = cpu_.read_guest_u32(vtable + kVtableExecute) & ~3u;
        }
        note_entry(entry);

        // PREDICTED delivery (see the model in the JSON notes): the task's
        // first FP instruction traps; OSSwitchFPUContext saves the owner (if
        // any) and reloads the CURRENT context only when FPSAVED is set.
        bool significant = false;
        u64 intended = 0;
        bool intended_known = false;
        u64 task_f1 = live_f31;
        if (o.cls == kOwnerCurrent) {
            ++wrong_f1_.owner_current_harmless;
            intended = live_f31;
            intended_known = true;
        } else {
            if (o.fpsaved) {
                task_f1 = o.ctx_f1;
                intended = o.ctx_f31;  // the thread's f31, saved when it lost the FPU
                intended_known = true;
                ++wrong_f1_.evaluated;
                if (task_f1 != intended) {
                    ++wrong_f1_.differs;
                    significant = true;
                } else {
                    ++wrong_f1_.same;
                }
                // The audit's literal test, kept for continuity. Live f31 is
                // the owner's (or garbage) here, so it is the wrong reference.
                if (o.ctx_f1 != live_f31) ++wrong_f1_.audit_formula_differs;
            } else {
                ++wrong_f1_.unknown_no_fpsaved;
            }
            if (o.cls == kOwnerOther) {
                ++clobber_execute_.events;
                if (live_f1 != live_f31) {
                    ++clobber_execute_.value_changed;
                    significant = true;
                }
            }
        }
        Event e = base_event(site, s, o, traversal_id);
        e.node = node;
        e.entry = entry;
        e.live_f1 = live_f1;
        e.live_f31 = live_f31;
        e.intended = intended;
        e.intended_known = intended_known;
        e.task_f1 = task_f1;
        e.task_f1_known = true;
        record(e, started, significant);
    }

    // hle_continuation(), after the frame identity check, before advance().
    void on_continuation(const FpDispatchRecord& dispatched, u64 traversal_id) noexcept {
        ++continuations_;
        const int slot = dispatched.valid ? fp_step_slot(dispatched.step) : -1;
        if (slot < 0) {
            ++unattributed_;
            return;
        }
        const std::size_t index = static_cast<std::size_t>(slot);
        const FpSite site = kFpContinuationSite[index];
        const Sample s = sample();
        const bool started = account(site, s);
        StepCounters& c = by_step_[index];
        ++c.n;
        const bool fp_at_dispatch = (dispatched.msr & kMsrFP) != 0u;
        const bool trapped = s.fp_exc != dispatched.fp_exceptions;
        if (!s.fp_on) ++c.fp_off;
        if (fp_at_dispatch && !s.fp_on) ++c.fp_lost_during_call;
        if (!fp_at_dispatch) {
            ++c.fp_off_already_at_dispatch;
            if (trapped) {
                ++c.dispatched_fp_off_trapped_during_call;
            } else {
                ++c.dispatched_fp_off_not_trapped_during_call;
            }
        }
        if (trapped) ++c.fp_exceptions_during_call;
        if (s.d4 != dispatched.d4) ++c.ctx_changed_during_call;
        if (s.d8 != dispatched.d8) ++c.owner_changed_during_call;
        if (s.fp_on) return;
        const Owner o = classify(s);
        note_owner(site, s, o);
        Event e = base_event(site, s, o, traversal_id);
        e.node = dispatched.node;
        e.has_dispatch = true;
        e.msr_at_dispatch = dispatched.msr;
        e.d4_at_dispatch = dispatched.d4;
        e.d8_at_dispatch = dispatched.d8;
        e.fp_exc_at_dispatch = dispatched.fp_exceptions;
        e.live_f1 = cpu_.fpr[1].bits[0];
        e.live_f31 = cpu_.fpr[31].bits[0];
        record(e, started, false);
    }

    // finish(), after the frame's f31 is read and before it is written.
    void on_finish(u32 frame, u64 saved_f31, bool nested, u64 traversal_id) noexcept {
        const FpSite site = nested ? FpSite::FinishNested : FpSite::Finish;
        ++finishes_[nested ? 1 : 0];
        const u32 link = cpu_.read_guest_u32(frame + kFrameSavedLink);
        const bool caller_is_traversal = link >= kTraversal && link < kTraversalEnd;
        if (caller_is_traversal) ++finish_caller_guest_traversal_[nested ? 1 : 0];
        const Sample s = sample();
        const bool started = account(site, s);
        if (s.fp_on) return;
        const Owner o = classify(s);
        note_owner(site, s, o);
        const u64 live_f31 = cpu_.fpr[31].bits[0];
        FinishCounters& f = finish_f31_[nested ? 1 : 0];
        bool significant = false;
        if (o.cls == kOwnerCurrent) {
            ++f.owner_current_harmless;
        } else {
            // PREDICTED: the caller's next FP instruction traps and the load
            // reloads f31 from +0x188 when FPSAVED is set, discarding the
            // frame value; without FPSAVED nothing is reloaded.
            if (o.fpsaved) {
                if (o.ctx_f31 != saved_f31) {
                    ++f.caller_wrong;
                    significant = true;
                } else {
                    ++f.caller_same;
                }
            } else {
                ++f.no_reload_frame_value_kept;
            }
            if (o.cls == kOwnerOther) {
                ++clobber_finish_[nested ? 1 : 0].events;
                if (live_f31 != saved_f31) {
                    ++clobber_finish_[nested ? 1 : 0].value_changed;
                    significant = true;
                }
            }
        }
        Event e = base_event(site, s, o, traversal_id);
        e.link = link;
        e.live_f1 = cpu_.fpr[1].bits[0];
        e.live_f31 = live_f31;
        e.frame_f31 = saved_f31;
        e.intended = saved_f31;
        e.intended_known = true;
        record(e, started, significant);
    }

    // Graph::note_paused_skip / Graph::leave_level (nested levels only).
    void observe(FpSite site) noexcept {
        if (site == FpSite::PausedSkip) {
            ++paused_skips_;
        } else if (site == FpSite::LeaveNested) {
            ++nested_level_exits_;
        }
        (void)account(site, sample());
    }

    void note_state_load() noexcept {
        ++state_loads_;
        episode_open_ = false;
        episode_length_ = 0;
        have_last_ = false;
    }

    std::string json() const;
    std::string summary() const;

private:
    static constexpr u8 kOwnerZero = 0;
    static constexpr u8 kOwnerCurrent = 1;
    static constexpr u8 kOwnerOther = 2;
    static constexpr std::size_t kMaxEvents = 256;
    static constexpr u64 kEventsPerEpisode = 8;  // then only significant events
    static constexpr std::size_t kMaxOwnerPairs = 32;
    static constexpr std::size_t kMaxEntries = 64;

    struct Sample {
        u32 msr = 0;
        u32 d4 = 0;
        u32 d8 = 0;
        u64 fp_exc = 0;
        bool fp_on = false;
    };
    struct Owner {
        u8 cls = kOwnerZero;
        bool ctx_valid = false;
        bool fpsaved = false;
        bool owner_is_thread = false;
        bool current_is_thread = false;
        bool threads_truncated = false;
        u32 threads_active = 0;
        u32 e4 = 0;
        u64 ctx_f1 = 0;
        u64 ctx_f31 = 0;
    };
    struct SiteCounters {
        u64 n = 0, fp_off = 0, episode_starts = 0;
        u64 own_zero = 0, own_current = 0, own_other = 0, own_other_is_thread = 0;
    };
    struct StepCounters {
        u64 n = 0, fp_off = 0, fp_lost_during_call = 0, fp_off_already_at_dispatch = 0;
        u64 dispatched_fp_off_trapped_during_call = 0;
        u64 dispatched_fp_off_not_trapped_during_call = 0;
        u64 fp_exceptions_during_call = 0;
        u64 ctx_changed_during_call = 0, owner_changed_during_call = 0;
    };
    struct WrongF1 {
        u64 evaluated = 0, differs = 0, same = 0, unknown_no_fpsaved = 0;
        u64 owner_current_harmless = 0, audit_formula_differs = 0;
    };
    struct FinishCounters {
        u64 caller_wrong = 0, caller_same = 0, no_reload_frame_value_kept = 0;
        u64 owner_current_harmless = 0;
    };
    struct Clobber {
        u64 events = 0, value_changed = 0;
    };
    struct OwnerPair {
        u32 current = 0, owner = 0;
        bool owner_is_thread = false;
        u64 n = 0;
    };
    struct EntryCount {
        u32 entry = 0;
        u64 n = 0;
    };
    struct Event {
        u64 seq = 0;
        FpSite site = FpSite::Begin;
        bool episode_start = false;
        bool significant = false;
        u64 episode_observation = 0;
        u64 traversal_id = 0;
        u32 node = 0, entry = 0, link = 0;
        u32 msr = 0, d4 = 0, d8 = 0, e4 = 0;
        bool has_dispatch = false;
        u32 msr_at_dispatch = 0, d4_at_dispatch = 0, d8_at_dispatch = 0;
        u64 fp_exc = 0, fp_exc_at_dispatch = 0;
        bool ctx_valid = false, fpsaved = false;
        u8 owner_class = kOwnerZero;
        u32 threads_active = 0;
        bool owner_is_thread = false, current_is_thread = false, threads_truncated = false;
        u64 live_f1 = 0, live_f31 = 0, ctx_f1 = 0, ctx_f31 = 0, frame_f31 = 0;
        u64 intended = 0, task_f1 = 0;
        bool intended_known = false, task_f1_known = false;
        u64 instr = 0;
        bool has_vi = false;
        u64 vi_frame = 0;
    };

    static u64 fp_exceptions() noexcept {
        return cpu_diag().exceptions_by_vector[kFpVectorSlot];
    }

    u64 peek64(u32 address) noexcept {
        return (static_cast<u64>(cpu_.read_guest_u32(address)) << 32) |
               cpu_.read_guest_u32(address + 4u);
    }

    Sample sample() noexcept {
        Sample s;
        s.msr = cpu_.msr();
        s.fp_on = (s.msr & kMsrFP) != 0u;
        s.d4 = cpu_.read_guest_u32(kOsCurrentContext);
        s.d8 = cpu_.read_guest_u32(kOsFpuContext);
        s.fp_exc = fp_exceptions();
        return s;
    }

    // Counts one observation and runs the episode model. Native never repairs
    // MSR[FP], so one loss is seen at many later sites; legacy traps once at the
    // first guarded FP instruction and then runs with FP = 1 until something
    // takes it away again. An episode therefore closes on FP = 1, on any
    // FP-unavailable exception since the previous observation (the task's own
    // trap repaired FP, and a later callback may have lost it again), or on a
    // change of the current / FPU-owner context words. An FP = 0 observation at
    // a site with a guarded FP instruction, while closed, opens one: that is the
    // count comparable to a legacy trap at that SRR0 (an estimate; fp_off is the
    // upper bound). Returns true when this observation opened an episode.
    bool account(FpSite site, const Sample& s) noexcept {
        const std::size_t index = static_cast<std::size_t>(site);
        SiteCounters& c = sites_[index];
        ++c.n;
        ++observations_;
        if (have_last_ && (s.fp_exc != last_fp_exc_ || s.d4 != last_d4_ || s.d8 != last_d8_)) {
            if (episode_open_) {
                episode_open_ = false;
                ++episodes_closed_by_change_;
            }
        }
        have_last_ = true;
        last_fp_exc_ = s.fp_exc;
        last_d4_ = s.d4;
        last_d8_ = s.d8;
        if (s.fp_on) {
            if (episode_open_) {
                episode_open_ = false;
                ++episodes_closed_by_fp_on_;
            }
            return false;
        }
        ++c.fp_off;
        bool started = false;
        if (!episode_open_) {
            if (kFpSiteInfo[index].legacy_srr0 != 0u) {
                episode_open_ = true;
                episode_length_ = 0;
                ++c.episode_starts;
                ++episodes_;
                started = true;
            } else {
                ++fp_off_nonlegacy_while_closed_;
            }
        }
        if (episode_open_) {
            ++episode_length_;
            episode_max_observations_ = std::max(episode_max_observations_, episode_length_);
        }
        return started;
    }

    Owner classify(const Sample& s) noexcept {
        if (!fp_handler_read_) {
            fp_handler_read_ = true;
            fp_handler_word_ = cpu_.read_guest_u32(kOsFpHandlerSlot);
        }
        Owner o;
        o.e4 = cpu_.read_guest_u32(kOsCurrentThread);
        o.ctx_valid = (s.d4 & 3u) == 0u && cpu_.guest_memory().mapped(GcAddr(s.d4), 0x2D0u);
        if (s.d8 == 0u) {
            o.cls = kOwnerZero;
        } else if (s.d8 == s.d4) {
            o.cls = kOwnerCurrent;
        } else {
            o.cls = kOwnerOther;
        }
        if (o.ctx_valid) {
            o.fpsaved = (cpu_.read_guest_u8(s.d4 + kCtxStateLo) & 1u) != 0u;
            o.ctx_f1 = peek64(s.d4 + kCtxF1);
            o.ctx_f31 = peek64(s.d4 + kCtxF31);
        }
        u32 thread = cpu_.read_guest_u32(kOsActiveThreadHead);
        u32 walked = 0;
        while (thread != 0u && walked < kThreadWalkCap) {
            if (thread == s.d8) o.owner_is_thread = true;
            if (thread == s.d4) o.current_is_thread = true;
            ++walked;
            thread = cpu_.read_guest_u32(thread + kThreadActiveNext);
        }
        o.threads_active = walked;
        o.threads_truncated = thread != 0u;
        return o;
    }

    void note_owner(FpSite site, const Sample& s, const Owner& o) noexcept {
        SiteCounters& c = sites_[static_cast<std::size_t>(site)];
        if (o.cls == kOwnerZero) {
            ++c.own_zero;
        } else if (o.cls == kOwnerCurrent) {
            ++c.own_current;
        } else {
            ++c.own_other;
            if (o.owner_is_thread) ++c.own_other_is_thread;
        }
        const u32 current = s.d4;
        const u32 owner = s.d8;
        for (std::size_t i = 0; i < owner_pair_count_; ++i) {
            if (owner_pairs_[i].current == current && owner_pairs_[i].owner == owner) {
                ++owner_pairs_[i].n;
                return;
            }
        }
        if (owner_pair_count_ < kMaxOwnerPairs) {
            owner_pairs_[owner_pair_count_++] = {current, owner, o.owner_is_thread, 1u};
        } else {
            ++owner_pairs_overflow_;
        }
    }

    void note_entry(u32 entry) noexcept {
        for (std::size_t i = 0; i < entry_count_; ++i) {
            if (entries_[i].entry == entry) {
                ++entries_[i].n;
                return;
            }
        }
        if (entry_count_ < kMaxEntries) {
            entries_[entry_count_++] = {entry, 1u};
        } else {
            ++entries_overflow_;
        }
    }

    Event base_event(FpSite site, const Sample& s, const Owner& o, u64 traversal_id) {
        Event e;
        e.seq = observations_;
        e.site = site;
        e.episode_observation = episode_open_ ? episode_length_ : 0u;
        e.traversal_id = traversal_id;
        e.msr = s.msr;
        e.d4 = s.d4;
        e.d8 = s.d8;
        e.e4 = o.e4;
        e.fp_exc = s.fp_exc;
        e.ctx_valid = o.ctx_valid;
        e.fpsaved = o.fpsaved;
        e.owner_class = o.cls;
        e.threads_active = o.threads_active;
        e.owner_is_thread = o.owner_is_thread;
        e.current_is_thread = o.current_is_thread;
        e.threads_truncated = o.threads_truncated;
        e.ctx_f1 = o.ctx_f1;
        e.ctx_f31 = o.ctx_f31;
        e.instr = cpu_.game_clock().now();
        if (vi_frame_) {
            e.has_vi = true;
            e.vi_frame = vi_frame_();
        }
        return e;
    }

    // Bounded: an episode logs its first few observations, then only the
    // evidentiary ones (a predicted wrong value or a changed foreign register).
    void record(Event& e, bool started, bool significant) noexcept {
        e.episode_start = started;
        e.significant = significant;
        if (!started && !significant && episode_open_ && episode_length_ > kEventsPerEpisode) {
            ++events_skipped_long_episode_;
            return;
        }
        if (event_count_ < kMaxEvents) {
            events_[event_count_++] = e;
        } else {
            ++events_dropped_;
        }
    }

    Cpu& cpu_;
    std::function<u64()> vi_frame_;
    u64 enabled_at_ticks_ = 0;
    u64 fp_exceptions_at_enable_ = 0;
    bool fpr_shadow_at_enable_ = false;

    std::array<SiteCounters, kFpSiteCount> sites_{};
    std::array<StepCounters, kFpStepSlots> by_step_{};
    std::array<u64, kFpStepSlots> dispatches_{};
    WrongF1 wrong_f1_;
    std::array<FinishCounters, 2> finish_f31_{};  // [0] outermost, [1] nested
    Clobber clobber_execute_;
    std::array<Clobber, 2> clobber_finish_{};
    std::array<u64, 2> finishes_{};
    std::array<u64, 2> finish_caller_guest_traversal_{};

    u64 begins_ = 0, begin_fp_off_ = 0, executes_ = 0, instrumented_ = 0;
    u64 continuations_ = 0, unattributed_ = 0, paused_skips_ = 0, nested_level_exits_ = 0;
    u64 state_loads_ = 0, observations_ = 0;

    bool episode_open_ = false;
    u64 episode_length_ = 0;
    u64 episodes_ = 0, episode_max_observations_ = 0;
    u64 episodes_closed_by_change_ = 0, episodes_closed_by_fp_on_ = 0;
    u64 fp_off_nonlegacy_while_closed_ = 0;
    bool have_last_ = false;
    u64 last_fp_exc_ = 0;
    u32 last_d4_ = 0, last_d8_ = 0;

    bool fp_handler_read_ = false;
    u32 fp_handler_word_ = 0;

    std::array<OwnerPair, kMaxOwnerPairs> owner_pairs_{};
    std::size_t owner_pair_count_ = 0;
    u64 owner_pairs_overflow_ = 0;
    std::array<EntryCount, kMaxEntries> entries_{};
    std::size_t entry_count_ = 0;
    u64 entries_overflow_ = 0;

    std::array<Event, kMaxEvents> events_{};
    std::size_t event_count_ = 0;
    u64 events_dropped_ = 0;
    u64 events_skipped_long_episode_ = 0;
};

const char* fp_owner_name(u8 cls) {
    switch (cls) {
    case 0: return "zero";
    case 1: return "current";
    default: return "other";
    }
}

std::string FpProbe::json() const {
    std::string out;
    out.reserve(96u * 1024u);
    const bool shadow_now = cpu_.interrupt_fpr_shadow();
    appendf(out,
            "{\"item\":\"r3\",\"installed\":true,\"scheduler\":\"native\","
            "\"enabled_at_game_ticks\":%llu,\"game_ticks_at_report\":%llu,"
            "\"state_loads\":%llu,",
            ull(enabled_at_ticks_), ull(cpu_.game_clock().now()), ull(state_loads_));
    // The reload model assumes rfi does not restore host FPRs. --fpr-shadow
    // restores them, which invalidates every wrong-f1 / caller-f31 prediction.
    appendf(out, "\"fpr_shadow\":%s,\"fpr_shadow_at_enable\":%s,",
            shadow_now ? "true" : "false", fpr_shadow_at_enable_ ? "true" : "false");
    if (fp_handler_read_) {
        appendf(out, "\"fp_handler_word\":\"0x%08X\",\"fp_handler_expected\":\"0x%08X\",",
                fp_handler_word_, kOsSwitchFpuContext);
    } else {
        appendf(out, "\"fp_handler_word\":null,\"fp_handler_expected\":\"0x%08X\",",
                kOsSwitchFpuContext);
    }
    appendf(out,
            "\"fp_unavailable_exceptions\":{\"at_enable\":%llu,\"at_report\":%llu,"
            "\"since_enable\":%llu},",
            ull(fp_exceptions_at_enable_), ull(fp_exceptions()),
            ull(fp_exceptions() - fp_exceptions_at_enable_));

    appendf(out,
            "\"totals\":{\"observations\":%llu,\"begins\":%llu,\"begin_fp_off\":%llu,"
            "\"begin_guest_body_fallbacks\":%llu,\"executes\":%llu,\"instrumented\":%llu,"
            "\"continuations\":%llu,\"finishes\":%llu,\"finishes_nested\":%llu,"
            "\"finish_caller_guest_traversal\":%llu,\"finish_nested_caller_guest_traversal\":%llu,"
            "\"paused_skips\":%llu,\"nested_level_exits\":%llu,"
            "\"unattributed_continuations\":%llu,\"dispatches\":{",
            ull(observations_), ull(begins_), ull(begin_fp_off_), ull(begin_fp_off_),
            ull(executes_), ull(instrumented_), ull(continuations_), ull(finishes_[0]),
            ull(finishes_[1]), ull(finish_caller_guest_traversal_[0]),
            ull(finish_caller_guest_traversal_[1]), ull(paused_skips_),
            ull(nested_level_exits_), ull(unattributed_));
    for (std::size_t i = 0; i < kFpStepSlots; ++i) {
        appendf(out, "%s\"%s\":%llu", i == 0 ? "" : ",", kFpStepNames[i], ull(dispatches_[i]));
    }
    out += "}},";

    out += "\"sites\":{";
    for (std::size_t i = 0; i < kFpSiteCount; ++i) {
        const SiteCounters& c = sites_[i];
        const FpSiteInfo& info = kFpSiteInfo[i];
        appendf(out, "%s\"%s\":{", i == 0 ? "" : ",", info.name);
        if (info.legacy_srr0 != 0u) {
            appendf(out, "\"legacy_srr0\":\"0x%08X\",", info.legacy_srr0);
        } else {
            out += "\"legacy_srr0\":null,";
        }
        appendf(out,
                "\"executed\":%s,\"n\":%llu,\"fp_off\":%llu,\"episode_starts\":%llu",
                c.n != 0u ? "true" : "false", ull(c.n), ull(c.fp_off), ull(c.episode_starts));
        if (info.detail) {
            appendf(out,
                    ",\"owner\":{\"zero\":%llu,\"current\":%llu,\"other\":%llu,"
                    "\"other_is_thread\":%llu}",
                    ull(c.own_zero), ull(c.own_current), ull(c.own_other),
                    ull(c.own_other_is_thread));
        }
        if (static_cast<FpSite>(i) == FpSite::ExecuteInstrumented) {
            out += ",\"note\":\"dead path: 0x805EF274 has no writer; n=0 means unexercised, "
                   "not clean\"";
        }
        out += "}";
    }
    out += "},";

    out += "\"continuation_by_step\":{";
    for (std::size_t i = 0; i < kFpStepSlots; ++i) {
        const StepCounters& c = by_step_[i];
        appendf(out,
                "%s\"%s\":{\"executed\":%s,\"n\":%llu,\"fp_off\":%llu,"
                "\"fp_lost_during_call\":%llu,\"fp_off_already_at_dispatch\":%llu,"
                "\"dispatched_fp_off_trapped_during_call\":%llu,"
                "\"dispatched_fp_off_not_trapped_during_call\":%llu,"
                "\"fp_exceptions_during_call\":%llu,\"ctx_changed_during_call\":%llu,"
                "\"owner_changed_during_call\":%llu}",
                i == 0 ? "" : ",", kFpStepNames[i], c.n != 0u ? "true" : "false", ull(c.n),
                ull(c.fp_off), ull(c.fp_lost_during_call), ull(c.fp_off_already_at_dispatch),
                ull(c.dispatched_fp_off_trapped_during_call),
                ull(c.dispatched_fp_off_not_trapped_during_call),
                ull(c.fp_exceptions_during_call), ull(c.ctx_changed_during_call),
                ull(c.owner_changed_during_call));
    }
    out += "},";

    // Per legacy guard: the native sites that stand for it. fp_off is the
    // upper bound, episode_starts the estimate. native_real_traps are the traps
    // the NATIVE arm itself raises there (begin()'s fallback at ECB8); they are
    // also in the native arm's r13 histogram, together with the guards the
    // fallback's guest body raises.
    out += "\"legacy_equivalent\":{";
    for (std::size_t g = 0; g < 6; ++g) {
        const u32 srr0 = kLegacyFpGuards[g];
        u64 n = 0;
        u64 fp_off = 0;
        u64 starts = 0;
        u64 starts_excluding_begin = 0;
        for (std::size_t i = 0; i < kFpSiteCount; ++i) {
            if (kFpSiteInfo[i].legacy_srr0 != srr0) continue;
            n += sites_[i].n;
            fp_off += sites_[i].fp_off;
            starts += sites_[i].episode_starts;
            if (static_cast<FpSite>(i) != FpSite::Begin) {
                starts_excluding_begin += sites_[i].episode_starts;
            }
        }
        const u64 real_traps = srr0 == 0x8004ECB8u ? begin_fp_off_ : 0u;
        appendf(out,
                "%s\"0x%08X\":{\"n\":%llu,\"fp_off\":%llu,\"episode_starts\":%llu,"
                "\"episode_starts_excluding_begin\":%llu,\"native_real_traps\":%llu}",
                g == 0 ? "" : ",", srr0, ull(n), ull(fp_off), ull(starts),
                ull(starts_excluding_begin), ull(real_traps));
    }
    out += "},";

    appendf(out,
            "\"episodes\":{\"started\":%llu,\"max_observations\":%llu,"
            "\"closed_by_fp_on\":%llu,\"closed_by_exception_or_context_change\":%llu,"
            "\"open_at_report\":%s,\"fp_off_nonlegacy_while_closed\":%llu},",
            ull(episodes_), ull(episode_max_observations_), ull(episodes_closed_by_fp_on_),
            ull(episodes_closed_by_change_), episode_open_ ? "true" : "false",
            ull(fp_off_nonlegacy_while_closed_));

    appendf(out,
            "\"wrong_f1\":{\"predicted\":true,\"evaluated\":%llu,\"differs\":%llu,"
            "\"same\":%llu,\"unknown_no_fpsaved\":%llu,\"owner_current_harmless\":%llu,"
            "\"audit_formula_differs\":%llu},",
            ull(wrong_f1_.evaluated), ull(wrong_f1_.differs), ull(wrong_f1_.same),
            ull(wrong_f1_.unknown_no_fpsaved), ull(wrong_f1_.owner_current_harmless),
            ull(wrong_f1_.audit_formula_differs));
    out += "\"finish_f31\":{\"predicted\":true";
    for (std::size_t k = 0; k < 2; ++k) {
        const FinishCounters& f = finish_f31_[k];
        appendf(out,
                ",\"%s\":{\"caller_wrong\":%llu,\"caller_same\":%llu,"
                "\"no_reload_frame_value_kept\":%llu,\"owner_current_harmless\":%llu}",
                k == 0 ? "outermost" : "nested", ull(f.caller_wrong), ull(f.caller_same),
                ull(f.no_reload_frame_value_kept), ull(f.owner_current_harmless));
    }
    out += "},";
    appendf(out,
            "\"clobber_other_context\":{\"execute_f1\":{\"events\":%llu,\"value_changed\":%llu},"
            "\"finish_f31_outermost\":{\"events\":%llu,\"value_changed\":%llu},"
            "\"finish_f31_nested\":{\"events\":%llu,\"value_changed\":%llu}},",
            ull(clobber_execute_.events), ull(clobber_execute_.value_changed),
            ull(clobber_finish_[0].events), ull(clobber_finish_[0].value_changed),
            ull(clobber_finish_[1].events), ull(clobber_finish_[1].value_changed));

    out += "\"owner_pairs\":[";
    for (std::size_t i = 0; i < owner_pair_count_; ++i) {
        const OwnerPair& p = owner_pairs_[i];
        appendf(out, "%s{\"current\":\"0x%08X\",\"owner\":\"0x%08X\",\"owner_is_thread\":%s,"
                     "\"n\":%llu}",
                i == 0 ? "" : ",", p.current, p.owner, p.owner_is_thread ? "true" : "false",
                ull(p.n));
    }
    appendf(out, "],\"owner_pairs_overflow\":%llu,", ull(owner_pairs_overflow_));
    out += "\"fp_off_execute_entries\":[";
    for (std::size_t i = 0; i < entry_count_; ++i) {
        appendf(out, "%s{\"entry\":\"0x%08X\",\"n\":%llu}", i == 0 ? "" : ",",
                entries_[i].entry, ull(entries_[i].n));
    }
    appendf(out, "],\"entries_overflow\":%llu,", ull(entries_overflow_));

    appendf(out,
            "\"events_recorded\":%llu,\"events_dropped\":%llu,"
            "\"events_skipped_long_episode\":%llu,\"events\":[",
            ull(event_count_), ull(events_dropped_), ull(events_skipped_long_episode_));
    for (std::size_t i = 0; i < event_count_; ++i) {
        const Event& e = events_[i];
        appendf(out,
                "%s{\"seq\":%llu,\"site\":\"%s\",\"episode_start\":%s,\"significant\":%s,"
                "\"episode_observation\":%llu,\"traversal_id\":%llu,\"node\":\"0x%08X\","
                "\"entry\":\"0x%08X\",\"link\":\"0x%08X\",\"msr\":\"0x%08X\","
                "\"d4\":\"0x%08X\",\"d8\":\"0x%08X\",\"e4\":\"0x%08X\",",
                i == 0 ? "" : ",", ull(e.seq), kFpSiteInfo[static_cast<std::size_t>(e.site)].name,
                e.episode_start ? "true" : "false", e.significant ? "true" : "false",
                ull(e.episode_observation), ull(e.traversal_id), e.node, e.entry, e.link, e.msr,
                e.d4, e.d8, e.e4);
        if (e.has_dispatch) {
            appendf(out,
                    "\"msr_at_dispatch\":\"0x%08X\",\"d4_at_dispatch\":\"0x%08X\","
                    "\"d8_at_dispatch\":\"0x%08X\",\"fp_exc_at_dispatch\":%llu,",
                    e.msr_at_dispatch, e.d4_at_dispatch, e.d8_at_dispatch,
                    ull(e.fp_exc_at_dispatch));
        } else {
            out += "\"msr_at_dispatch\":null,\"d4_at_dispatch\":null,\"d8_at_dispatch\":null,"
                   "\"fp_exc_at_dispatch\":null,";
        }
        appendf(out,
                "\"fp_exc\":%llu,\"ctx_valid\":%s,\"fpsaved\":%s,\"owner_class\":\"%s\","
                "\"threads_active\":%u,\"threads_truncated\":%s,\"owner_is_thread\":%s,"
                "\"current_is_thread\":%s,\"live_f1\":\"0x%016llX\",\"live_f31\":\"0x%016llX\","
                "\"ctx_f1\":\"0x%016llX\",\"ctx_f31\":\"0x%016llX\",\"frame_f31\":\"0x%016llX\",",
                ull(e.fp_exc), e.ctx_valid ? "true" : "false", e.fpsaved ? "true" : "false",
                fp_owner_name(e.owner_class), e.threads_active,
                e.threads_truncated ? "true" : "false", e.owner_is_thread ? "true" : "false",
                e.current_is_thread ? "true" : "false", ull(e.live_f1), ull(e.live_f31),
                ull(e.ctx_f1), ull(e.ctx_f31), ull(e.frame_f31));
        if (e.intended_known) {
            appendf(out, "\"intended\":\"0x%016llX\",", ull(e.intended));
        } else {
            out += "\"intended\":null,";
        }
        if (e.task_f1_known) {
            appendf(out, "\"predicted_task_f1\":\"0x%016llX\",", ull(e.task_f1));
        } else {
            out += "\"predicted_task_f1\":null,";
        }
        appendf(out, "\"instr\":%llu,", ull(e.instr));
        if (e.has_vi) {
            appendf(out, "\"vi_frame\":%llu}", ull(e.vi_frame));
        } else {
            out += "\"vi_frame\":null}";
        }
    }
    out += "],";

    out += "\"notes\":\"STEP-0 observational probe; guest state untouched. Nested-level ECB8 "
           "(prologue of an inner level) has no site: it follows ED24/EDA8 in the same host run "
           "with no recognition point, so its FP state is theirs. ED80 path is dead (0x805EF274 "
           "has no writer). ps1 (fpr bits[1]) is not measured. wrong_f1/finish_f31 are "
           "PREDICTIONS of OSSwitchFPUContext (handler slot 7 = fp_handler_word, expected "
           "0x80373100): owner!=current and FPSAVED set reloads f1 from ctx+0x98 and f31 from "
           "ctx+0x188; the intended execute f1 is ctx+0x188 (the thread's saved f31), not live "
           "f31 (audit_formula_differs keeps the audit's literal test). Invalid if fpr_shadow is "
           "true. Episode = legacy-trap estimate: closes on FP=1, on any FP-unavailable "
           "exception, or on a 0xD4/0xD8 change; fp_off is the upper bound. Compare the legacy "
           "arm's r13 FP-unavailable count at SRR0 X with (native legacy_equivalent[X]"
           ".episode_starts_excluding_begin + the native arm's own r13 count at X): the native "
           "r13 count includes begin()'s fallback trap at ECB8 and the guards raised by that "
           "fallback's guest body. Arms diverge after the first differing trap; compare rates, "
           "not events.\"}";
    return out;
}

std::string FpProbe::summary() const {
    std::string out;
    const u64 fp_off_execute =
        sites_[static_cast<std::size_t>(FpSite::Execute)].fp_off +
        sites_[static_cast<std::size_t>(FpSite::ExecuteInstrumented)].fp_off;
    const u64 fp_off_finish = sites_[static_cast<std::size_t>(FpSite::Finish)].fp_off +
                              sites_[static_cast<std::size_t>(FpSite::FinishNested)].fp_off;
    appendf(out,
            "[r3] executes %llu (fp_off %llu) continuations %llu (fp_off: Execute %llu Mark %llu "
            "Destroy %llu Yield %llu) finishes %llu+%llu nested (fp_off %llu) begin traps %llu\n",
            ull(executes_ + instrumented_), ull(fp_off_execute), ull(continuations_),
            ull(by_step_[0].fp_off + by_step_[1].fp_off), ull(by_step_[2].fp_off),
            ull(by_step_[3].fp_off), ull(by_step_[4].fp_off), ull(finishes_[0]),
            ull(finishes_[1]), ull(fp_off_finish), ull(begin_fp_off_));
    u64 zero = 0;
    u64 current = 0;
    u64 other = 0;
    u64 other_thread = 0;
    for (const SiteCounters& c : sites_) {
        zero += c.own_zero;
        current += c.own_current;
        other += c.own_other;
        other_thread += c.own_other_is_thread;
    }
    appendf(out,
            "[r3] fp_off owners: zero %llu current %llu other %llu (thread %llu); predicted "
            "wrong-f1 differs %llu/%llu evaluated; caller-f31 wrong %llu outer %llu nested; "
            "other-context clobbers exec %llu (changed %llu) finish %llu (changed %llu)\n",
            ull(zero), ull(current), ull(other), ull(other_thread), ull(wrong_f1_.differs),
            ull(wrong_f1_.evaluated), ull(finish_f31_[0].caller_wrong),
            ull(finish_f31_[1].caller_wrong), ull(clobber_execute_.events),
            ull(clobber_execute_.value_changed),
            ull(clobber_finish_[0].events + clobber_finish_[1].events),
            ull(clobber_finish_[0].value_changed + clobber_finish_[1].value_changed));
    u64 starts[6] = {};
    for (std::size_t g = 0; g < 6; ++g) {
        for (std::size_t i = 0; i < kFpSiteCount; ++i) {
            if (kFpSiteInfo[i].legacy_srr0 == kLegacyFpGuards[g] &&
                static_cast<FpSite>(i) != FpSite::Begin) {
                starts[g] += sites_[i].episode_starts;
            }
        }
    }
    appendf(out,
            "[r3] legacy-equivalent episode starts ECB8 %llu (+%llu real begin traps) ED24 %llu "
            "ED80 %llu ED94 %llu EDA8 %llu EDD8 %llu; fp-unavailable exceptions since enable "
            "%llu\n",
            ull(starts[0]), ull(begin_fp_off_), ull(starts[1]), ull(starts[2]), ull(starts[3]),
            ull(starts[4]), ull(starts[5]), ull(fp_exceptions() - fp_exceptions_at_enable_));
    if (cpu_.interrupt_fpr_shadow() || fpr_shadow_at_enable_) {
        out += "[r3] WARNING: --fpr-shadow is on; rfi restores host FPRs, so the wrong-f1 / "
               "caller-f31 predictions do not apply to this run\n";
    }
    if (fp_handler_read_ && fp_handler_word_ != kOsSwitchFpuContext) {
        appendf(out,
                "[r3] WARNING: FP-unavailable handler slot reads 0x%08X, not OSSwitchFPUContext "
                "0x%08X; the reload model does not apply\n",
                fp_handler_word_, kOsSwitchFpuContext);
    }
    if (begins_ == 0u) {
        out += "[r3] UNEXERCISED: native begin() never ran (hook replaced or no gameplay)\n";
    }
    return out;
}

}  // namespace

// Diagnostic (GEKKO_TASK_TIMES=<csv>, docs/GPU_FILL.md "stage start"): host steady-clock nanoseconds at every task
// dispatch (its node and vtable) and at every return to the scheduler's continuation, so a task's own time is the
// span from its dispatch to the next event. Off unless the variable is set; written when the scheduler is destroyed.
struct TaskTimeLog {
    struct Event {
        u64 ns;
        u32 node, vtable, depth, kind;  // kind 0 = execute, 1 = continuation reached, 2 = mark, 3 = destroy
    };
    std::string path;
    std::vector<Event> events;
    TaskTimeLog() {
        if (const char* p = std::getenv("GEKKO_TASK_TIMES"); p != nullptr && p[0] != 0) {
            path = p;
            events.reserve(std::size_t{1} << 20);
        }
    }
    bool on() const noexcept { return !path.empty(); }
    void note(u32 kind, u32 node, u32 vtable, std::size_t depth) {
        const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count());
        events.push_back(Event{ns, node, vtable, static_cast<u32>(depth), kind});
    }
    ~TaskTimeLog() {
        if (path.empty())
            return;
        std::FILE* out = std::fopen(path.c_str(), "wb");
        if (out == nullptr)
            return;
        std::fputs("ns,kind,node,vtable,depth\n", out);
        for (const Event& e : events)
            std::fprintf(out, "%llu,%u,%08X,%08X,%u\n", static_cast<unsigned long long>(e.ns), e.kind, e.node,
                         e.vtable, e.depth);
        std::fclose(out);
    }
};

struct GameplayScheduler::Impl {
    TaskTimeLog task_times;
    // GameplayScheduler::set_boundary_hook: the outermost traversal completed.
    std::function<void()> boundary_hook;
    // The guest side of the shared traversal: every node is an address and
    // every field access is a guest load or store, so the scheduler and the
    // game are looking at the same bytes.
    struct Graph {
        using Node = u32;
        Impl& owner;

        Node none() const noexcept { return 0u; }
        bool valid(Node node) const noexcept { return node != 0u; }
        Node first_child(Node node) { return link(node + kFirstChild); }
        Node next_sibling(Node node) { return link(node + kNextSibling); }
        std::uint16_t flags(Node node) { return owner.cpu.mem_read16(node + kFlags); }
        void set_flags(Node node, std::uint16_t value) {
            owner.cpu.mem_write16(node + kFlags, value);
        }
        bool paused() { return owner.cpu.mem_read8(kPauseState) != 0u; }
        bool instrumented() { return owner.cpu.mem_read32(kProfilerArmed) != 0u; }
        void note_visit(Node) noexcept { ++owner.stats.nodes_visited; }
        void note_paused_skip(Node) noexcept {
            ++owner.stats.paused_skips;
            // STEP-0 R3 measurement (default off): L_8004ED70 -> L_8004EDA8.
            if (owner.fp_measure) owner.fp_probe->observe(FpSite::PausedSkip);
        }
        bool yield_at_back_edge() { return owner.cpu.needs_dispatch(); }
        bool ready(shadow::gameplay::Checkpoint point, Node node) {
            using shadow::gameplay::Checkpoint;
            if (point == Checkpoint::ExecuteArguments) owner.cpu.gpr[3] = node;
            if ((owner.cpu.msr() & kMsrFP) == 0u) {
                auto& traversal = *owner.active.back();
                if (!traversal.fp_suspended) {
                    if (point == Checkpoint::RestoreCaller) {
                        owner.cpu.mem_write32(kRecursionDepth, owner.cpu.mem_read32(kRecursionDepth) - 1u);
                        traversal.return_started = true;
                    }
                    traversal.fp_suspended = true;
                }
                recomp::fp_unavailable(owner.cpu, kContinuation);
                return false;
            }
            owner.active.back()->fp_suspended = false;
            if (point == Checkpoint::DescendArguments || point == Checkpoint::MarkedArguments)
                owner.cpu.fpr[1].bits[0] = owner.cpu.fpr[31].bits[0];
            return true;
        }
        void enter_level(Node node) {
            owner.cpu.mem_write32(kRecursionDepth, owner.cpu.mem_read32(kRecursionDepth) + 1u);
            ++owner.stats.levels;
            owner.open_levels = owner.open_levels + 1u;
            owner.stats.level_peak = std::max(owner.stats.level_peak, owner.open_levels);
#if defined(SHADOW_TRACY)
            // The per-traversal peak. stats.level_peak is monotone over the
            // whole run, so as a series it is a flat line; this one is reset
            // per outermost call and can move. No Tracy event is emitted here -
            // this site runs about 41k times a second.
            owner.tracy_level_peak = std::max(owner.tracy_level_peak, owner.open_levels);
#endif
            (void)node;
        }
        void leave_level(Node node) {
            if (!owner.active.back()->return_started)
                owner.cpu.mem_write32(kRecursionDepth, owner.cpu.mem_read32(kRecursionDepth) - 1u);
            owner.active.back()->return_started = false;
            if (owner.open_levels != 0u) --owner.open_levels;
            (void)node;
            // STEP-0 R3 measurement (default off): a nested level's epilogue
            // (lfd f31 at 0x8004EDD8). pop_level pops before calling here, so
            // depth 0 is the outermost pop, which finish() observes; unwind()
            // runs only after a fault or a stop and is not a guest epilogue.
            if (owner.fp_measure && !owner.faulted && !owner.cpu.stopped() &&
                !owner.active.empty() && owner.active.back()->walk.depth() != 0u) {
                owner.fp_probe->observe(FpSite::LeaveNested);
            }
        }

    private:
        // A link that does not address a task object is not guessed at: the
        // machine stops with the address that broke, rather than reading MMIO
        // or walking into a tree that is not there.
        Node link(u32 field) {
            const u32 value = owner.cpu.mem_read32(field);
            if (value != 0u && !task_address_ok(owner.memory, value)) {
                owner.fault("task link 0x%08X at 0x%08X does not address a task object",
                            value, field);
                return 0u;
            }
            return value;
        }
    };

    struct Traversal {
        explicit Traversal(Graph& graph) : walk(graph) {}
        shadow::gameplay::Walk<Graph> walk;
        u32 frame = 0;   // guest stack pointer of this call's frame
        u64 id = 0;
        bool return_started = false;
        bool fp_suspended = false;
        FpDispatchRecord fp;  // STEP-0 R3 measurement only; written only when enabled
    };

    Cpu& cpu;
    GuestMemory& memory;
    Graph graph{*this};
    std::vector<std::unique_ptr<Traversal>> active;
    // Traversal owns the only Gameplay-domain container in this scheduler.
    // The guest contract already refuses more than 64 concurrent traversals,
    // so retaining one owner per observed nesting slot is both the exact
    // ownership bound and enough to keep Walk::frames_ capacity across calls.
    std::vector<std::unique_ptr<Traversal>> idle;
    Stats stats;
    u64 open_levels = 0;
    u64 next_id = 1;
    bool installed = false;
    bool faulted = false;

    // STEP-0 R3 measurement (default off). One predictable branch per site
    // while disabled; see FpProbe.
    bool fp_measure = false;
    std::unique_ptr<FpProbe> fp_probe;

#if defined(SHADOW_TRACY)
    // Profiling-only state: the shipped binary is the one the pixel gate hashes
    // and the one a paired A/B measures, so none of this may exist in it.
    bool tracy_frame_open = false;
    Stats tracy_baseline;      // counters as they stood when the outermost call began
    u64 tracy_level_peak = 0;  // this traversal's deepest level (see enter_level)
#endif

    Impl(Cpu& c, GuestMemory& b) : cpu(c), memory(b) {
        active.reserve(64u);
        idle.reserve(64u);
    }

    std::unique_ptr<Traversal> acquire_traversal() {
        if (!idle.empty()) {
            auto traversal = std::move(idle.back());
            idle.pop_back();
            ++stats.traversal_storage_reused;
            return traversal;
        }
        ++stats.traversal_storage_created;
        stats.traversal_storage_peak =
            std::max<u64>(stats.traversal_storage_peak,
                          static_cast<u64>(active.size() + idle.size() + 1u));
        return std::make_unique<Traversal>(graph);
    }

    void recycle_top() noexcept {
        auto traversal = std::move(active.back());
        active.pop_back();
        idle.push_back(std::move(traversal));
    }

#if defined(SHADOW_TRACY)
    // A budget-bounded run ends mid-traversal far more often than not, so the
    // last gameplay frame is usually still open here.
    ~Impl() { tracy_close_frame(); }

    // One point per completed outermost traversal, for every counter the walk
    // already keeps. A zone per node or per dispatched task is out of the
    // question at ~41k/s, and would be a lie besides: dispatch_execute only
    // loads the registers and returns, so the task body runs AFTER the zone
    // would have closed. paused_skips is here because it is the only per-node
    // work refusal this subsystem has - without it, a pause-policy regression
    // reads as executes falling while visits stay flat, with nothing to explain
    // the gap.
    void tracy_publish() {
        const Stats& base = tracy_baseline;
        GEKKO_PROFILE_PLOT("cpu/sched-visits",
                           static_cast<double>(stats.nodes_visited - base.nodes_visited));
        GEKKO_PROFILE_PLOT("cpu/sched-executes",
                           static_cast<double>((stats.executes + stats.instrumented) -
                                               (base.executes + base.instrumented)));
        // Every one of these five is incremented exactly once on a path that
        // ends in leave_to_dispatcher, so the sum IS the hand-back count the
        // native scheduler's protocol is supposed to produce.
        GEKKO_PROFILE_PLOT("cpu/sched-round-trips",
                           static_cast<double>((stats.executes + stats.instrumented +
                                                stats.destroys + stats.marks + stats.yields) -
                                               (base.executes + base.instrumented +
                                                base.destroys + base.marks + base.yields)));
        GEKKO_PROFILE_PLOT("cpu/sched-teardown",
                           static_cast<double>((stats.destroys + stats.marks) -
                                               (base.destroys + base.marks)));
        // A plot rather than a message: yield_at_back_edge is polled twice per
        // visited node, so a message site's ceiling is the node rate, while
        // this one is bounded by the traversal rate however many yields occur.
        GEKKO_PROFILE_PLOT("cpu/sched-yields", static_cast<double>(stats.yields - base.yields));
        GEKKO_PROFILE_PLOT("cpu/sched-paused-skips",
                           static_cast<double>(stats.paused_skips - base.paused_skips));
        GEKKO_PROFILE_PLOT("cpu/sched-depth", static_cast<double>(tracy_level_peak));
    }

    // Closes the gameplay frame from any of the ways a traversal can end.
    // Balance is tracked by the flag rather than inferred from `active`, which
    // two of those paths have already emptied by the time they get here.
    void tracy_close_frame() {
        if (!tracy_frame_open) return;
        tracy_frame_open = false;
        GEKKO_PROFILE_FRAME_END("cpu/task-tree");
    }
#endif

    template <class... Args>
    void fault(const char* format, Args... args) {
        if (faulted) return;
        faulted = true;
        ++stats.faults;
        char detail[256];
        if constexpr (sizeof...(Args) == 0) {
            // A bare message: printed as text (clang's -Wformat-security refuses a non-literal format alone).
            std::snprintf(detail, sizeof(detail), "%s", format);
        } else {
            std::snprintf(detail, sizeof(detail), format, args...);
        }
        char message[320];
        std::snprintf(message, sizeof(message), "native gameplay scheduler: %s", detail);
        // The text is already built for request_stop and Tracy copies it, so
        // the refusal costs nothing extra to put on the timeline. It is NOT
        // latched for the lifetime of the run: discard_traversals() clears
        // `faulted` on every state load, so the bound is one per load plus one.
        GEKKO_PROFILE_MESSAGE(message, std::strlen(message));
        cpu.request_stop(StopReason::ExecutionFault, message);
    }

    bool ram_ok(u32 address, u32 size) const { return memory.mapped(GcAddr(address), size); }

    // ---- the guest call boundary ------------------------------------------
    //
    // A task body is never called on the host stack. It is handed to the
    // dispatcher with the link register pointing at kContinuation, so control
    // comes back through hle_continuation after any interrupt the call made
    // deliverable has been taken - the same recognition point the original
    // reaches by returning into its own frame.
    void dispatch(u32 target, u32 site) {
        // With a translation table attached, a task body that has none is a
        // refusal here rather than a fault later with no context. A build with
        // no table attached at all is the reference engine, which interprets.
        if (cpu.recompiled() != nullptr && cpu.recompiled()->lookup(target) == nullptr &&
            cpu.recompiled()->find_range(target) == nullptr) {
            fault("task entry 0x%08X (from 0x%08X) has no translated code", target, site);
            return;
        }
        cpu.lr = kContinuation;
        cpu.handoff_native_call(target, kContinuation);
    }

    void dispatch_execute(u32 node, bool instrumented) {
        cpu.gpr[3] = node;
        // STEP-0 R3 measurement (default off): observes the unguarded write
        // below (the original guards it at 0x8004ED94 / 0x8004ED80).
        if (fp_measure) fp_probe->on_execute(node, instrumented, active.back()->id);
        // The frame delta lives in f31 for the whole traversal, where the
        // original's prologue put it.
        cpu.fpr[1].bits[0] = cpu.fpr[31].bits[0];
        if (instrumented) {
            ++stats.instrumented;
            dispatch(kProfilingCall, 0x8004ED88u);
            return;
        }
        const u32 vtable = cpu.mem_read32(node + kVtable);
        if (!ram_ok(vtable, 16u)) {
            fault("task 0x%08X has vtable 0x%08X outside RAM", node, vtable);
            return;
        }
        const u32 entry = cpu.mem_read32(vtable + kVtableExecute);
        cpu.gpr[12] = entry;
        cpu.ctr = entry;
        if (task_times.on())
            task_times.note(0u, node, vtable, active.size());
        ++stats.executes;
        dispatch(entry & ~3u, 0x8004EDA4u);
    }

    void dispatch_destroy(u32 node) {
        const u32 vtable = cpu.mem_read32(node + kVtable);
        if (!ram_ok(vtable, 16u)) {
            fault("task 0x%08X has vtable 0x%08X outside RAM", node, vtable);
            return;
        }
        const u32 entry = cpu.mem_read32(vtable + kVtableDestructor);
        cpu.gpr[3] = node;
        cpu.gpr[4] = 1u;  // the original's "release the storage too" argument
        cpu.gpr[12] = entry;
        cpu.ctr = entry;
        ++stats.destroys;
        dispatch(entry & ~3u, 0x8004ED58u);
    }

    void dispatch_mark(u32 node) {
        cpu.gpr[3] = node;
        ++stats.marks;
        dispatch(kMarkChildren, 0x8004ED20u);
    }

    // ---- traversal lifetime ------------------------------------------------

    bool begin(u32 parent) {
        // Scoped over the whole body, ahead of the first check, so all four
        // refusals (bad parent, no stack room, 64 concurrent calls, a stale
        // frame) and the FP-unavailable resume are inside the zone rather than
        // silently outside it. Safe to span `return advance()`: leaving to the
        // dispatcher sets a flag and returns normally, it does not unwind.
        GEKKO_PROFILE_ZONE("cpu/sched-begin");
        if (!task_address_ok(memory, parent)) {
            fault("traversal requested for 0x%08X, which is not a task object", parent);
            return true;
        }
        const u32 stack = cpu.gpr[1];
        if (stack < kFrameSize || !ram_ok(stack - kFrameSize, kFrameSize + 8u)) {
            fault("no room for a scheduler frame below stack 0x%08X", stack);
            return true;
        }
        // The original nests one guest frame per manager-driven pump; a run
        // that keeps nesting is a broken continuation, not deep gameplay.
        if (active.size() >= 64u) {
            fault("%zu scheduler calls are already in progress", active.size());
            return true;
        }
        const u32 frame = stack - kFrameSize;
        // A nested call always sits below the frame of the call it interrupted.
        // Anything else means an outstanding traversal never came back - a task
        // that left through a non-local exit - and resuming it would resume a
        // walk of a tree whose caller is gone.
        if (!active.empty() && frame >= active.back()->frame) {
            fault("traversal at frame 0x%08X is stale; a new one starts at 0x%08X",
                  active.back()->frame, frame);
            return true;
        }
        // The original's frame, field for field. The first three instructions
        // (stwu / mfspr LR / stw) come first and on their own, because the
        // fourth is `stfd f31,24(r1)`: with MSR[FP] clear the machine takes a
        // floating-point-unavailable exception THERE, with the stack already
        // pushed and the link already saved. Resuming at 0x8004ECB8 then lands
        // in the original body with the frame the original built, and that call
        // completes as the guest traversal. Checking the FPU before building
        // the frame would resume it into a frame that was never pushed.
        cpu.mem_write32(frame, stack);
        cpu.gpr[1] = frame;
        cpu.mem_write32(frame + kFrameSavedLink, cpu.lr);
        // STEP-0 R3 measurement (default off).
        if (fp_measure) fp_probe->on_begin(parent, next_id);
        if ((cpu.msr() & kMsrFP) == 0u) {
            recomp::fp_unavailable(cpu, 0x8004ECB8u);
            return true;
        }
        auto traversal = acquire_traversal();
        traversal->frame = frame;
        traversal->id = next_id++;
        traversal->return_started = false;
        traversal->fp_suspended = false;
        traversal->fp = FpDispatchRecord{};
        cpu.mem_write32(frame + kFrameSavedR29, cpu.gpr[29]);
        cpu.mem_write32(frame + kFrameSavedR29 + 4u, cpu.gpr[30]);
        cpu.mem_write32(frame + kFrameSavedR29 + 8u, cpu.gpr[31]);
        cpu.mem_write64(frame + kFrameSavedF31, cpu.fpr[31].bits[0]);
        cpu.fpr[31].bits[0] = cpu.fpr[1].bits[0];
        cpu.gpr[29] = parent;
        ++stats.traversals;
        stats.nested_peak = std::max<u64>(stats.nested_peak, active.size() + 1u);
        active.push_back(std::move(traversal));
#if defined(SHADOW_TRACY)
        // The outermost traversal IS the gameplay frame. A nested call (legal
        // to 64) must not rebase the outer walk's deltas or wipe its level
        // peak, because open_levels is one Impl-wide counter and not per
        // traversal - hence the size()==1 guard on the snapshot as well as on
        // the frame mark. Taken before advance(), which can finish and publish.
        if (active.size() == 1u) {
            tracy_baseline = stats;
            tracy_level_peak = 0;
            tracy_frame_open = true;
            GEKKO_PROFILE_FRAME_START("cpu/task-tree");
        }
#endif
        // Concurrent traversal depth against the hard refusal limit of 64.
        // Deliberately published at both ends and on every nesting level: this
        // is the only series that shows the nesting the guards above assume.
        GEKKO_PROFILE_PLOT("cpu/sched-active", static_cast<double>(active.size()));
        active.back()->walk.begin(parent);
        return advance();
    }

    // Returns the whole traversal to its caller: the original's epilogue,
    // register for register.
    void finish() {
        Traversal& traversal = *active.back();
        const u32 frame = traversal.frame;
        // The one production read, in the same order as before; the probe
        // (STEP-0 R3, default off) sees the live f31 before it is replaced.
        const u64 saved_f31 = cpu.mem_read64(frame + kFrameSavedF31);
        if (fp_measure) fp_probe->on_finish(frame, saved_f31, active.size() > 1u, traversal.id);
        cpu.fpr[31].bits[0] = saved_f31;
        cpu.gpr[29] = cpu.mem_read32(frame + kFrameSavedR29);
        cpu.gpr[30] = cpu.mem_read32(frame + kFrameSavedR29 + 4u);
        cpu.gpr[31] = cpu.mem_read32(frame + kFrameSavedR29 + 8u);
        cpu.lr = cpu.mem_read32(frame + kFrameSavedLink);
        cpu.gpr[1] = frame + kFrameSize;
        recycle_top();
        GEKKO_PROFILE_PLOT("cpu/sched-active", static_cast<double>(active.size()));
        if (active.empty() && boundary_hook) {
            // The call is served: the guest resumes at its return address. Set here because a stop the hook asks
            // for (a savestate boundary) makes Cpu::try_hle_slow leave pc alone, and a machine saved with pc at the
            // continuation stub would re-enter the scheduler with no traversal when it is restored.
            cpu.pc = cpu.lr;
            boundary_hook();
        }
#if defined(SHADOW_TRACY)
        // The publish block: one set of points per completed outermost walk.
        if (active.empty()) {
            tracy_publish();
            tracy_close_frame();
        }
#endif
    }

    // Runs the walk until it needs the guest, or until the call is over. Every
    // outcome hands control back: a task call and a back-edge yield go through
    // the dispatcher, and a finished traversal returns to its caller. Nothing
    // here keeps running guest work on the host stack.
    bool advance() {
        using shadow::gameplay::Step;
        if (active.empty()) {
            fault("continuation reached with no traversal in progress");
            return true;
        }
        if (task_times.on())
            task_times.note(1u, 0u, 0u, active.size());
        const auto action = active.back()->walk.run();
        if (faulted || cpu.stopped()) {
            abandon();
            return true;
        }
        // STEP-0 R3 measurement (default off): which step the continuation
        // that follows will be attributed to, and the FP state it left with.
        if (fp_measure && action.step != Step::Finished && action.step != Step::Suspended) {
            fp_probe->note_dispatch(active.back()->fp, action.step, action.node);
        }
        switch (action.step) {
        case Step::Suspended:
            return true;
        case Step::Finished:
            finish();
            return true;
        case Step::Yield:
            ++stats.yields;
            cpu.leave_to_dispatcher(kContinuation, FallbackReason::Yield, kContinuation);
            return true;
        case Step::Execute:
            dispatch_execute(action.node, false);
            break;
        case Step::ExecuteInstrumented:
            dispatch_execute(action.node, true);
            break;
        case Step::MarkChildren:
            dispatch_mark(action.node);
            break;
        case Step::Destroy:
            dispatch_destroy(action.node);
            break;
        }
        if (faulted) abandon();
        return true;
    }

    // A refusal already stopped the machine; close the levels that were open so
    // no guest flag is left in a half-updated state for a savestate to capture.
    void abandon() {
        while (!active.empty()) {
            active.back()->walk.unwind();
            recycle_top();
        }
        open_levels = 0;
#if defined(SHADOW_TRACY)
        // An abandoned walk publishes nothing - its counters are half a
        // traversal - but the frame it opened still has to be closed.
        tracy_close_frame();
#endif
    }

    bool hle_traversal(Cpu& c) {
        (void)c;
        return begin(cpu.gpr[3]);
    }

    bool hle_continuation(Cpu& c) {
        (void)c;
        if (active.empty()) {
            fault("a task returned to the scheduler with no traversal in progress");
            return true;
        }
        // The stack pointer is the identity: a task that returned normally
        // restored it to this traversal's frame. Nothing is read out of the
        // frame to check that, because every word of it is the callee's to use.
        const Traversal& traversal = *active.back();
        const u32 frame = cpu.gpr[1];
        if (frame != traversal.frame) {
            fault("task returned on stack 0x%08X, not the scheduler frame 0x%08X",
                  frame, traversal.frame);
            abandon();
            return true;
        }
        // STEP-0 R3 measurement (default off).
        if (fp_measure && !traversal.fp_suspended)
            fp_probe->on_continuation(traversal.fp, traversal.id);
        return advance();
    }
};

GameplayScheduler::GameplayScheduler(Cpu& cpu, GuestMemory& memory)
    : impl_(std::make_unique<Impl>(cpu, memory)) {}
GameplayScheduler::~GameplayScheduler() = default;

void GameplayScheduler::install() {
    if (impl_->installed) return;
    impl_->installed = true;
    Impl* impl = impl_.get();
    impl_->cpu.install_hle(
        kTraversal, [impl](Cpu& cpu) { return impl->hle_traversal(cpu); },
        "native gameplay scheduler");
    impl_->cpu.install_hle(
        kContinuation, [impl](Cpu& cpu) { return impl->hle_continuation(cpu); },
        "native gameplay scheduler continuation");
}

GameplayScheduler::Stats GameplayScheduler::stats() const noexcept { return impl_->stats; }

bool GameplayScheduler::busy() const noexcept { return !impl_->active.empty(); }

void GameplayScheduler::set_boundary_hook(std::function<void()> hook) {
    impl_->boundary_hook = std::move(hook);
}

void GameplayScheduler::discard_traversals() noexcept {
    while (!impl_->active.empty()) impl_->recycle_top();
    impl_->open_levels = 0;
    impl_->faulted = false;
    // STEP-0 R3 measurement: the probe survives the load; its episode does not.
    if (impl_->fp_probe) impl_->fp_probe->note_state_load();
#if defined(SHADOW_TRACY)
    // A state load drops the walks of a machine that no longer exists; the
    // frame they opened would otherwise stay open across the load.
    impl_->tracy_close_frame();
#endif
}

// ---- STEP-0 measurement, audit item R3 (default off) ------------------------

void GameplayScheduler::enable_fp_probe(std::function<u64()> vi_frame) {
    if (impl_->fp_probe) return;
    impl_->fp_probe = std::make_unique<FpProbe>(impl_->cpu, std::move(vi_frame));
    impl_->fp_measure = true;
}

bool GameplayScheduler::fp_probe_enabled() const noexcept {
    return impl_->fp_probe != nullptr;
}

std::string GameplayScheduler::fp_probe_json() const {
    return impl_->fp_probe ? impl_->fp_probe->json() : std::string("{}");
}

std::string GameplayScheduler::fp_probe_summary() const {
    return impl_->fp_probe ? impl_->fp_probe->summary() : std::string();
}

}  // namespace gekko
