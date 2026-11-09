#pragma once

// The one task-traversal algorithm, shared by every binding.
//
// The original game schedules its entire world through one recursive function
// (PAL main.dol 0x8004ECAC). Every task is a node in one tree; one call walks a
// subtree, runs each eligible node's virtual execute, destroys the nodes that
// asked to be removed, and recurses into children. 159 classes derive from the
// Task base, so this function is the game's frame.
//
// This header is that algorithm, written once, as a resumable state machine.
// Two things forced the resumable shape rather than an ordinary recursion:
//
//   * The driver may not be able to perform an action immediately. The guest
//     binding runs a task's execute by handing the address to the dispatcher
//     and returning; control comes back through a continuation hook, not
//     through a C++ return. A host call stack cannot survive that, so the walk
//     state lives here, in the frame stack, and never on the host stack.
//   * An interrupt must stay deliverable between two tasks, exactly where the
//     original left it deliverable. The original unwinds to the dispatcher at
//     each call boundary and resumes from guest registers; this state machine
//     reaches the same recognition points because run() returns there.
//
// The stages below are named after the original's basic blocks so the two can
// be compared line by line. Nothing here accounts instructions: game time is
// the game clock, which no walk advances (docs/native-time-20261005/DESIGN.md).
//
// Flag bits are the original's, for every binding:
//   0x0001  removal requested            0x000E  subtree disabled
//   0x0020  destructor already ran       0x0100  runs while the game is paused
//
// gameplay_scheduler_reference_tests drives both bindings against the original
// instructions executing in the reference interpreter and compares the callback
// and lifetime traces.

#include <cstddef>
#include <cstdint>

#include "native_memory.h"

namespace shadow::gameplay {

// What the walk needs the driver to do before it can go on.
enum class Step : std::uint8_t {
    Finished,             // the whole traversal is done
    Execute,              // call the node's execute with the frame delta
    ExecuteInstrumented,  // the same call through the game's own profiling wrapper
    MarkChildren,         // request removal of every descendant of the node
    Destroy,              // run the node's destructor; it has no children left
    Yield,                // a loop back edge the driver asked to be released at
    Suspended,
};

enum class Checkpoint : std::uint8_t {
    ExecuteArguments,
    InstrumentedArguments,
    DescendArguments,
    MarkedArguments,
    RestoreCaller,
};

// Bits the original tests. Named so a binding cannot invent its own meanings.
inline constexpr std::uint16_t kFlagRemovalRequested = 0x0001u;
inline constexpr std::uint16_t kFlagDisabledMask = 0x000Eu;
inline constexpr std::uint16_t kFlagDestroyed = 0x0020u;
inline constexpr std::uint16_t kFlagRunsWhilePaused = 0x0100u;

// A binding supplies the node type and these operations. Nothing here knows
// whether a node is a host object or a guest address.
//
//   using Node = ...;                       trivially copyable, equality-comparable
//   Node none() const;                      the empty node
//   bool valid(Node) const;
//   Node first_child(Node) const;
//   Node next_sibling(Node) const;
//   std::uint16_t flags(Node) const;
//   void set_flags(Node, std::uint16_t);
//   bool paused() const;                    the game pause state
//   bool instrumented() const;              the per-task profiler is armed
//   void note_visit(Node);                  one node reached by the walk
//   void note_paused_skip(Node);            a node the pause state suppressed
//   bool yield_at_back_edge();              release control here if something waits
//   void enter_level(Node parent);          one traversal level begins
//   void leave_level(Node parent);          ... and ends
template <class Graph>
class Walk {
public:
    using Node = typename Graph::Node;
    struct Action {
        Step step = Step::Finished;
        Node node{};
    };

    explicit Walk(Graph& graph) : graph_(graph) {}

    // Enter the outermost traversal level of one call.
    void begin(Node parent) {
        frames_.clear();
        push_level(parent);
    }
    bool active() const noexcept { return !frames_.empty(); }
    std::size_t depth() const noexcept { return frames_.size(); }

    // Abandon an unfinished traversal: a task threw, or the machine faulted
    // inside one. Every level that was open is closed in the order the
    // original would have unwound them, and a node whose removal was in
    // progress keeps its request so the next visit takes it again.
    void unwind() {
        while (!frames_.empty()) {
            const Frame frame = frames_.back();
            frames_.pop_back();
            graph_.leave_level(frame.parent);
            if (graph_.valid(frame.dying)) {
                graph_.set_flags(frame.dying,
                                 static_cast<std::uint16_t>(graph_.flags(frame.dying) |
                                                            kFlagRemovalRequested));
            }
        }
    }

    // Advance until the driver has to act, or until the traversal is over.
    // After performing the returned action the driver calls run() again.
    Action run() {
        for (;;) {
            if (frames_.empty()) return {Step::Finished, graph_.none()};
            Frame& frame = frames_.back();
            switch (frame.stage) {
            case Stage::LoopCondition:  // L_8004EDB8
                // The parent losing its last child ends the level even if the
                // cursor still names a sibling: the original tests the parent
                // child list first, and a destructor that emptied it has
                // already unlinked whatever the cursor named.
                if (!graph_.valid(graph_.first_child(frame.parent))) {
                    frame.stage = Stage::Return;
                    continue;
                }
                if (graph_.valid(frame.cursor)) {
                    frame.stage = Stage::Body;
                    // The original polls here, on the loop back edge, and hands
                    // control to the dispatcher when something has become
                    // deliverable. Releasing at the same point keeps interrupt
                    // latency where it was.
                    if (graph_.yield_at_back_edge()) return {Step::Yield, frame.cursor};
                    continue;
                }
                frame.stage = Stage::Return;
                continue;

            case Stage::Return:
                if (!graph_.ready(Checkpoint::RestoreCaller, frame.parent))
                    return {Step::Suspended, frame.parent};
                pop_level();
                continue;

            case Stage::Body: {  // L_8004ECDC
                graph_.note_visit(frame.cursor);
                const std::uint16_t flags = graph_.flags(frame.cursor);
                frame.flags = flags;
                if ((flags & (kFlagRemovalRequested | kFlagDisabledMask)) == 0u) {
                    frame.stage = Stage::Eligible;
                    continue;
                }
                // A disabled node with no removal request is skipped whole:
                // neither it nor its children are visited.
                if ((flags & kFlagRemovalRequested) == 0u) {
                    frame.stage = Stage::Advance;
                    continue;
                }
                // Removal is deferred, not cancelled, while paused: the request
                // bit is left set and reconsidered on a later visit.
                if (graph_.paused()) {
                    frame.stage = Stage::Advance;
                    continue;
                }
                graph_.set_flags(frame.cursor,
                                 static_cast<std::uint16_t>(flags & ~kFlagRemovalRequested));
                if ((graph_.flags(frame.cursor) & kFlagDestroyed) != 0u) {
                    frame.stage = Stage::Advance;
                    continue;
                }
                // The sibling to resume at is read before anything is destroyed.
                frame.dying = frame.cursor;
                frame.cursor = graph_.next_sibling(frame.dying);
                frame.stage = Stage::DestroySubtree;
                continue;
            }

            case Stage::DestroySubtree:  // L_8004ED30
                // Bottom-up: mark the children for removal and walk them away,
                // one generation per pass, before the node itself is destroyed.
                if (graph_.valid(graph_.first_child(frame.dying))) {
                    frame.stage = Stage::MarkChildrenCall;
                    if (graph_.yield_at_back_edge()) return {Step::Yield, frame.dying};
                    continue;
                }
                if (!graph_.valid(frame.dying)) {
                    frame.stage = Stage::LoopCondition;
                    continue;
                }
                frame.stage = Stage::AfterDestroy;
                return {Step::Destroy, frame.dying};

            case Stage::MarkChildrenCall:  // L_8004ED1C
                frame.stage = Stage::AfterMarkChildren;
                return {Step::MarkChildren, frame.dying};

            case Stage::AfterMarkChildren: {  // L_8004ED24
                if (!graph_.ready(Checkpoint::MarkedArguments, frame.dying))
                    return {Step::Suspended, frame.dying};
                frame.stage = Stage::DestroySubtree;
                const Node dying = frame.dying;
                push_level(dying);
                continue;
            }

            case Stage::AfterDestroy:  // L_8004ED5C
                frame.stage = Stage::LoopCondition;
                continue;

            case Stage::Eligible:  // L_8004ED60
                if (!graph_.paused()) {
                    frame.stage = Stage::SelectCall;
                    continue;
                }
                // Paused and not exempt: the node does not run, but its
                // children are still visited.
                if ((frame.flags & kFlagRunsWhilePaused) == 0u) {
                    graph_.note_paused_skip(frame.cursor);
                    frame.stage = Stage::Descend;
                    continue;
                }
                frame.stage = Stage::SelectCall;
                continue;

            case Stage::SelectCall:  // L_8004ED74
                if (!graph_.instrumented()) {
                    frame.stage = Stage::Call;
                    continue;
                }
                frame.stage = Stage::InstrumentedArguments;
                continue;

            case Stage::InstrumentedArguments:
                if (!graph_.ready(Checkpoint::InstrumentedArguments, frame.cursor))
                    return {Step::Suspended, frame.cursor};
                frame.stage = Stage::AfterInstrumentedCall;
                return {Step::ExecuteInstrumented, frame.cursor};

            case Stage::Call:  // L_8004ED90
                if (!graph_.ready(Checkpoint::ExecuteArguments, frame.cursor))
                    return {Step::Suspended, frame.cursor};
                frame.stage = Stage::Descend;
                return {Step::Execute, frame.cursor};

            case Stage::AfterInstrumentedCall:  // L_8004ED8C
                frame.stage = Stage::Descend;
                continue;

            case Stage::Descend: {  // L_8004EDA8
                if (!graph_.ready(Checkpoint::DescendArguments, frame.cursor))
                    return {Step::Suspended, frame.cursor};
                frame.stage = Stage::Advance;
                const Node node = frame.cursor;
                push_level(node);
                continue;
            }

            case Stage::Advance:  // L_8004EDB4
                // Re-read after the subtree ran: the next sibling is whatever
                // the link says now, not what it said before.
                frame.cursor = graph_.next_sibling(frame.cursor);
                frame.stage = Stage::LoopCondition;
                continue;
            }
        }
    }

private:
    enum class Stage : std::uint8_t {
        LoopCondition,
        Body,
        DestroySubtree,
        MarkChildrenCall,
        AfterMarkChildren,
        AfterDestroy,
        Eligible,
        SelectCall,
        Call,
        InstrumentedArguments,
        AfterInstrumentedCall,
        Descend,
        Advance,
        Return,
    };
    struct Frame {
        Node parent{};
        Node cursor{};
        Node dying{};
        std::uint16_t flags = 0;
        Stage stage = Stage::LoopCondition;
    };

    void push_level(Node parent) {
        graph_.enter_level(parent);
        Frame frame;
        frame.parent = parent;
        frame.cursor = graph_.first_child(parent);
        frame.dying = graph_.none();
        frame.stage = Stage::LoopCondition;
        frames_.push_back(frame);
    }
    void pop_level() {
        const Node parent = frames_.back().parent;
        frames_.pop_back();
        graph_.leave_level(parent);
    }

    Graph& graph_;
    memory::Vector<Frame, memory::Domain::Gameplay> frames_;
};

}  // namespace shadow::gameplay
