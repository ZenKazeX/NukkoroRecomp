#include "../include/native_gameplay.h"

#include "../include/gameplay_traversal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace shadow::gameplay {
namespace {
std::atomic<std::uint64_t> next_id{1};
TaskId allocate_id() {
    auto value = next_id.load(std::memory_order_relaxed);
    for (;;) {
        if (value == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("task identity exhausted");
        if (next_id.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return {value};
    }
}
constexpr const char* names[] = {
    "System", "Audio", "Debug", "Scene", "PostSystem", "Render", "Manager", "Controller", "Command",
    "CharaColli", "Landscape", "Gadget", "Vehicle", "Player", "Enemy", "PostManager", "Particle", "Editor", "Camera"
};
constexpr Layer persistent_layers[] = {Layer::System, Layer::Audio, Layer::Debug, Layer::Scene, Layer::PostSystem, Layer::Render};
constexpr Layer scene_layers[] = {Layer::Manager, Layer::Controller, Layer::Command, Layer::Collision,
    Layer::Landscape, Layer::Gadget, Layer::Vehicle, Layer::Enemy, Layer::Player,
    Layer::PostManager, Layer::Particle, Layer::Editor, Layer::Camera};
// The original disables a subtree with any of bits 0x000E; one bit is enough
// to express it and the mask is what the traversal tests.
constexpr std::uint16_t kDisabled = 0x0002u;
}

struct TaskSystem::Impl {
    struct Node {
        TaskId id;
        std::string name;
        std::uint16_t flags = 0;
        std::shared_ptr<Task> task;
        Node* parent = nullptr;
        Node* previous = nullptr;
        Node* next = nullptr;
        Node* first = nullptr;
        Node* last = nullptr;
        bool pumping = false;
    };

    // The native side of the shared traversal. Nodes are raw links, as the
    // original's are; a node destroyed mid-traversal stays allocated until the
    // outermost walk ends (retired_), so a link the original would have read
    // out of freed guest memory reads the same stale node here instead of
    // being undefined behaviour.
    struct Graph {
        using Node = Impl::Node*;
        Impl& owner;
        Node none() const noexcept { return nullptr; }
        bool valid(Node node) const noexcept { return node != nullptr; }
        Node first_child(Node node) const noexcept { return node->first; }
        Node next_sibling(Node node) const noexcept { return node->next; }
        std::uint16_t flags(Node node) const noexcept { return node->flags; }
        void set_flags(Node node, std::uint16_t value) noexcept { node->flags = value; }
        bool paused() const noexcept { return owner.paused; }
        bool instrumented() const noexcept { return false; }
        bool ready(Checkpoint, Node) const noexcept { return true; }
        void note_visit(Node) const noexcept {}
        void note_paused_skip(Node) const noexcept { ++owner.counters.paused_updates; }
        // Native tasks run on their owning thread with nothing to hand back to.
        bool yield_at_back_edge() const noexcept { return false; }
        void enter_level(Node node) {
            if (node->pumping) throw std::logic_error("recursive gameplay subtree update");
            node->pumping = true;
            owner.counters.peak_depth = std::max(owner.counters.peak_depth, ++owner.depth);
        }
        void leave_level(Node node) noexcept {
            node->pumping = false;
            --owner.depth;
        }
    };

    TaskSystem& api;
    execution::Executor& jobs;
    const std::thread::id owner = std::this_thread::get_id();
    std::unordered_map<std::uint64_t, std::shared_ptr<Node>> nodes;
    std::vector<std::shared_ptr<Node>> retired;
    std::shared_ptr<Node> root;
    std::array<TaskId, static_cast<std::size_t>(Layer::Count)> layers{};
    Stats counters;
    Graph graph{*this};
    std::size_t depth = 0;
    std::size_t walks = 0;
    bool paused = false;
    bool closing = false;

    Impl(TaskSystem& system, execution::Executor& executor) : api(system), jobs(executor) {}

    void check_owner() const {
        if (std::this_thread::get_id() != owner) throw std::logic_error("gameplay mutation must run on its owning thread");
    }
    std::shared_ptr<Node> lookup(TaskId id) const {
        const auto found = nodes.find(id.value);
        return found == nodes.end() ? nullptr : found->second;
    }
    static void detach(Node& node) noexcept {
        if (node.previous) node.previous->next = node.next;
        else if (node.parent) node.parent->first = node.next;
        if (node.next) node.next->previous = node.previous;
        else if (node.parent) node.parent->last = node.previous;
        node.parent = node.previous = node.next = nullptr;
    }
    static void attach(Node& node, Node& parent) noexcept {
        node.parent = &parent;
        node.previous = parent.last;
        node.next = nullptr;
        if (parent.last) parent.last->next = &node;
        else parent.first = &node;
        parent.last = &node;
    }
    std::shared_ptr<Node> insert(Node* parent, std::string name, TaskOptions options, std::shared_ptr<Task> task = {}) {
        if (closing || (parent && destroyed(*parent))) throw std::logic_error("cannot attach to a closing task owner");
        auto node = std::allocate_shared<Node>(memory::Allocator<Node, memory::Domain::Gameplay>{});
        node->id = allocate_id();
        node->name = std::move(name);
        node->flags = static_cast<std::uint16_t>((options.update_while_paused ? kFlagRunsWhilePaused : 0u) |
                                                 (options.enabled ? 0u : kDisabled));
        node->task = std::move(task);
        nodes.emplace(node->id.value, node);
        if (parent) attach(*node, *parent);
        ++counters.created;
        counters.peak_live = std::max(counters.peak_live, nodes.size());
        return node;
    }
    static bool destroyed(const Node& node) noexcept { return (node.flags & kFlagDestroyed) != 0u; }
    void build_scene_layers() {
        const auto scene = lookup(layers[static_cast<std::size_t>(Layer::Scene)]);
        if (!scene) throw std::logic_error("scene task owner is missing");
        for (auto layer : scene_layers) {
            const auto index = static_cast<std::size_t>(layer);
            layers[index] = insert(scene.get(), names[index], {true, true})->id;
        }
    }
    void mark_children(Node& parent) {
        memory::Vector<Node*, memory::Domain::Scratch> pending;
        for (Node* child = parent.first; child; child = child->next) pending.push_back(child);
        while (!pending.empty()) {
            Node* node = pending.back();
            pending.pop_back();
            node->flags = static_cast<std::uint16_t>(node->flags | kFlagRemovalRequested);
            for (Node* child = node->first; child; child = child->next) pending.push_back(child);
        }
    }
    void erase_leaf(Node* node) {
        if (node->first) throw std::logic_error("task children must be destroyed before their owner");
        auto owned = lookup(node->id);
        // The original's order: the derived destructor body runs first, and
        // only when it chains into the base does the base set the guard bit
        // (0x8004EF48) and unlink (0x8004EF50). A task destructor therefore
        // still sees itself as live, which is what it sees on the console.
        node->task.reset();
        node->flags = static_cast<std::uint16_t>(node->flags | kFlagDestroyed);
        detach(*node);
        nodes.erase(node->id.value);
        // Held until the outermost traversal ends: the walk above may still
        // hold links to it, exactly as the original holds guest addresses.
        if (owned) retired.push_back(std::move(owned));
        ++counters.destroyed;
    }

    // One traversal level of the shared algorithm, driven to completion here.
    // The native binding can act immediately, so every requested step is
    // performed in this loop rather than through a continuation.
    void pump(Node* parent, float delta) {
        if (!std::isfinite(delta) || delta < 0.0f) throw std::invalid_argument("invalid gameplay frame delta");
        Walk<Graph> walk(graph);
        ++walks;
        struct Guard {
            Impl& impl;
            Walk<Graph>& walk;
            bool done = false;
            ~Guard() {
                if (!done) walk.unwind();
                if (--impl.walks == 0) impl.retired.clear();
            }
        } guard{*this, walk};
        walk.begin(parent);
        for (;;) {
            const auto action = walk.run();
            if (action.step == Step::Finished) break;
            switch (action.step) {
            case Step::Execute:
            case Step::ExecuteInstrumented:
                if (action.node->task) {
                    UpdateContext context{api, jobs, action.node->id, delta, counters.frames};
                    action.node->task->update(context);
                    ++counters.updates;
                }
                break;
            case Step::MarkChildren:
                mark_children(*action.node);
                break;
            case Step::Destroy:
                erase_leaf(action.node);
                break;
            case Step::Yield:
            case Step::Suspended:
            case Step::Finished:
                break;
            }
        }
        guard.done = true;
    }
    // Not noexcept: this runs task destructors, and one of those may do what
    // the original permits - ask for another task - which this object refuses
    // while closing. Terminating the process over that is not a shutdown.
    void clear() {
        closing = true;
        try {
            while (root && root->first) {
                Node* node = root->first;
                while (node->first) node = node->first;
                erase_leaf(node);
            }
        } catch (const std::exception&) {
            // The remaining nodes are dropped below with their tasks.
        }
        nodes.clear();
        retired.clear();
        root.reset();
    }
};

TaskSystem::TaskSystem(execution::Executor& jobs) : impl_(std::make_unique<Impl>(*this, jobs)) {
    impl_->root = impl_->insert(nullptr, "Root", {true, true});
    for (auto layer : persistent_layers) {
        const auto index = static_cast<std::size_t>(layer);
        impl_->layers[index] = impl_->insert(impl_->root.get(), names[index], {true, true})->id;
    }
    impl_->build_scene_layers();
}
TaskSystem::~TaskSystem() { impl_->clear(); }

void TaskSystem::validate_parent(TaskId parent) const {
    impl_->check_owner();
    auto owner = impl_->lookup(parent);
    if (!owner || Impl::destroyed(*owner) || impl_->closing)
        throw std::invalid_argument("task requires a live native owner");
}
TaskId TaskSystem::add(TaskId parent, std::string name, TaskOptions options, std::shared_ptr<Task> task) {
    impl_->check_owner();
    auto owner = impl_->lookup(parent);
    if (!owner || !task) throw std::invalid_argument("task requires a live native owner and implementation");
    return impl_->insert(owner.get(), std::move(name), options, std::move(task))->id;
}
TaskId TaskSystem::root() const noexcept { return impl_->root->id; }
TaskId TaskSystem::layer(Layer value) const {
    impl_->check_owner();
    const auto index = static_cast<std::size_t>(value);
    if (index >= impl_->layers.size()) throw std::out_of_range("invalid gameplay layer");
    return impl_->layers[index];
}
Task* TaskSystem::find(TaskId id) const {
    impl_->check_owner();
    auto node = impl_->lookup(id);
    return node && !Impl::destroyed(*node) ? node->task.get() : nullptr;
}
bool TaskSystem::alive(TaskId id) const {
    impl_->check_owner();
    auto node = impl_->lookup(id);
    return node && !Impl::destroyed(*node);
}
TaskId TaskSystem::parent(TaskId id) const {
    impl_->check_owner();
    auto node = impl_->lookup(id);
    return node && node->parent ? node->parent->id : TaskId{};
}
std::vector<TaskId> TaskSystem::children(TaskId id) const {
    impl_->check_owner();
    auto node = impl_->lookup(id);
    if (!node) throw std::invalid_argument("invalid task owner");
    std::vector<TaskId> result;
    for (auto* child = node->first; child; child = child->next) result.push_back(child->id);
    return result;
}
bool TaskSystem::request_destroy(TaskId id) {
    impl_->check_owner();
    auto node = impl_->lookup(id);
    if (!node || node == impl_->root || Impl::destroyed(*node)) return false;
    node->flags = static_cast<std::uint16_t>(node->flags | kFlagRemovalRequested);
    return true;
}
bool TaskSystem::set_enabled(TaskId id, bool enabled) {
    impl_->check_owner();
    auto node = impl_->lookup(id);
    if (!node || Impl::destroyed(*node)) return false;
    node->flags = static_cast<std::uint16_t>(enabled ? (node->flags & ~kFlagDisabledMask)
                                                     : (node->flags | kDisabled));
    return true;
}
bool TaskSystem::set_pause_policy(TaskId id, bool update_while_paused) {
    impl_->check_owner();
    auto node = impl_->lookup(id);
    if (!node || Impl::destroyed(*node)) return false;
    node->flags = static_cast<std::uint16_t>(update_while_paused ? (node->flags | kFlagRunsWhilePaused)
                                                                 : (node->flags & ~kFlagRunsWhilePaused));
    return true;
}
bool TaskSystem::reparent(TaskId id, TaskId parent) {
    impl_->check_owner();
    auto node = impl_->lookup(id), owner = impl_->lookup(parent);
    if (!node || !owner || node == impl_->root || Impl::destroyed(*node) || Impl::destroyed(*owner)) return false;
    for (auto* ancestor = owner.get(); ancestor; ancestor = ancestor->parent)
        if (ancestor == node.get()) return false;
    Impl::detach(*node);
    Impl::attach(*node, *owner);
    return true;
}
void TaskSystem::request_destroy_children(TaskId parent) {
    impl_->check_owner();
    auto node = impl_->lookup(parent);
    if (!node) throw std::invalid_argument("invalid task owner");
    impl_->mark_children(*node);
}
void TaskSystem::set_paused(bool value) { impl_->check_owner(); impl_->paused = value; }
bool TaskSystem::paused() const { impl_->check_owner(); return impl_->paused; }
void TaskSystem::tick(float delta) {
    impl_->check_owner();
    if (impl_->root->pumping) throw std::logic_error("recursive gameplay frame update");
    if (!std::isfinite(delta) || delta < 0.0f) throw std::invalid_argument("invalid gameplay frame delta");
    ++impl_->counters.frames;
    impl_->pump(impl_->root.get(), delta);
}
void TaskSystem::update_children(TaskId parent, float delta) {
    impl_->check_owner();
    auto node = impl_->lookup(parent);
    if (!node) throw std::invalid_argument("invalid task owner");
    impl_->pump(node.get(), delta);
}
void TaskSystem::reset_scene() {
    impl_->check_owner();
    auto scene = impl_->lookup(layer(Layer::Scene));
    if (!scene || scene->pumping) throw std::logic_error("scene reset requires an inactive scene traversal");
    impl_->mark_children(*scene);
    impl_->pump(scene.get(), 0.0f);
    impl_->build_scene_layers();
}
TaskSystem::Stats TaskSystem::stats() const {
    impl_->check_owner();
    auto result = impl_->counters;
    result.live = impl_->nodes.size();
    return result;
}

}
