#pragma once

// The native gameplay scheduler: the game's frame, walked by native code.
//
// The original schedules the whole world through one recursive guest function
// (PAL main.dol 0x8004ECAC). It walks one task tree, runs each eligible node's
// virtual execute, destroys the nodes that asked to be removed, and recurses.
// 159 classes derive from the Task base, so that function is the game's frame:
//
//   0x801EB680  pump the root task's children   (called from the main loop)
//   0x801EB624  scene reset: mark the Scene subtree, walk it away, rebuild
//   0x801E4FA0 / 0x801E501C  manager-driven subtree pumps
//        |                          all four call
//   0x8004ECAC  the traversal      <-- replaced here
//        |  per node: execute (vtable +0x0C) / destructor (vtable +0x08)
//
// What this class replaces is the *scheduling*: the ordering, the pause policy,
// the removal protocol and the recursion now run as native x86-64 in
// shadow::gameplay::Walk (runtime/include/gameplay_traversal.h), the same
// algorithm the address-free native task system uses. What it does NOT replace
// is the task bodies: each node's execute and destructor are still the game's
// own translated code, called through the dispatcher. This is a native
// scheduler driving translated tasks, not a native gameplay port.
//
// The tree itself stays where the game keeps it. Task nodes are the guest's own
// 64-byte objects in MEM1 and the walk reads and writes those fields directly:
//
//   +0x04 u16 flags   0x0001 removal requested   0x000E subtree disabled
//                     0x0020 destructor ran      0x0100 runs while paused
//   +0x08 previous sibling (the first child's slot holds the last)
//   +0x0C next sibling          +0x10 owner          +0x14 first child
//   +0x18 vtable
//
// Nothing outside the traversal reads the scheduler's own state: a whole-image
// scan of the PAL DOL finds the recursion counter 0x805EF270 read at exactly
// two instructions, both inside 0x8004ECAC. The pause byte 0x805EF27C has 14
// readers elsewhere and two writers (0x801EB668 / 0x801EB674) and stays in
// guest memory where all of them see it. So there is no second copy of the
// hierarchy to keep in sync, and no aliasing hazard: the native scheduler and
// the game read the same bytes.
//
// The contract that makes the replacement exact rather than approximate (the
// walk accounts no instructions: game time is the game clock, which only a
// wait point moves - docs/native-time-20261005/DESIGN.md):
//
//   * Recognition points. A task's execute is not called on the host stack: it
//     is handed to the dispatcher with the link register pointing at this
//     class's continuation hook, so an interrupt stays deliverable between two
//     tasks exactly where the original left it deliverable. The walk state
//     lives in the heap, not in a host frame, which is what lets control leave
//     and come back mid-traversal. The original's two loop back edges poll and
//     hand back as well; yield_at_back_edge reproduces both.
//
// --legacy-scheduler runs the guest's own traversal instead, for A/B capture.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "gekko/types.h"

namespace gekko {

class Cpu;
class GuestMemory;

class GameplayScheduler {
public:
    GameplayScheduler(Cpu& cpu, GuestMemory& memory);
    ~GameplayScheduler();
    GameplayScheduler(const GameplayScheduler&) = delete;
    GameplayScheduler& operator=(const GameplayScheduler&) = delete;

    // Installs the HLE hooks. Idempotent; must run before the guest boots.
    void install();

    struct Stats {
        u64 traversals = 0;        // guest calls to the scheduler this run
        u64 levels = 0;            // traversal levels entered (the original's recursion)
        u64 nodes_visited = 0;
        u64 executes = 0;          // task execute calls dispatched
        u64 instrumented = 0;      // ... through the game's profiling wrapper
        u64 destroys = 0;          // task destructor calls dispatched
        u64 marks = 0;             // subtree removal requests dispatched
        u64 paused_skips = 0;      // nodes the pause state suppressed
        u64 yields = 0;            // back-edge hand-backs to the dispatcher
        u64 nested_peak = 0;       // deepest concurrent guest-initiated traversal
        u64 level_peak = 0;        // deepest task tree walked
        u64 faults = 0;            // refusals: the run stops, nothing is guessed
        u64 traversal_storage_created = 0;  // persistent traversal owners allocated
        u64 traversal_storage_reused = 0;   // begins served from the idle pool
        u64 traversal_storage_peak = 0;     // active + idle; hard-bounded by nesting
    };
    Stats stats() const noexcept;

    // True between a task call and its continuation: part of the frame's walk
    // is host state at that moment, so it is not a point to capture the machine
    // at. Savestates refuse it rather than writing a state that cannot resume.
    bool busy() const noexcept;

    // Called when the outermost traversal completes (busy() has just become
    // false): a point to capture the machine at. The hook may request a stop;
    // it must not touch guest state.
    void set_boundary_hook(std::function<void()> hook);

    // Drops every in-progress traversal without touching guest memory. Used
    // when the whole machine is replaced under this object - a state load -
    // where the walks belong to a machine that no longer exists.
    void discard_traversals() noexcept;

    // ---- STEP-0 measurement, audit item R3 (default OFF) -------------------
    //
    // findings-2026-09-23/HARDWARE_ASSUMPTION_AUDIT.md R3: the scheduler writes
    // guest f1 (task execute) and f31 (finish) without the MSR[FP] precondition
    // the original's FP instructions carry. The probe counts how often those
    // writes, and the continuation that follows each dispatched step, run with
    // MSR[FP] = 0, and classifies each such event by lazy-FPU ownership (OS
    // low-memory words 0x800000D4 / 0x800000D8). Purely observational: no guest
    // write, no retirement, no dispatch or recognition-point change, no hook.
    // Every guest read is Cpu::read_guest_u32 / read_guest_u8. Call before the
    // guest runs; idempotent. The optional callback supplies the VI frame count
    // for the event log (guest thread only).
    void enable_fp_probe(std::function<u64()> vi_frame = {});
    bool fp_probe_enabled() const noexcept;
    std::string fp_probe_json() const;     // one JSON object; "{}" when disabled
    std::string fp_probe_summary() const;  // "[r3] ..." lines; empty when disabled

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gekko
