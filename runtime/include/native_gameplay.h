#pragma once

#include "native_execution.h"
#include "native_memory.h"

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace shadow::gameplay {

enum class Layer : std::size_t {
    System, Audio, Debug, Scene, PostSystem, Render, Manager, Controller, Command,
    Collision, Landscape, Gadget, Vehicle, Player, Enemy, PostManager, Particle, Editor, Camera, Count
};

struct TaskId {
    std::uint64_t value = 0;
    explicit operator bool() const noexcept { return value != 0; }
    bool operator==(TaskId other) const noexcept { return value == other.value; }
    bool operator!=(TaskId other) const noexcept { return value != other.value; }
};

struct TaskOptions {
    bool update_while_paused = false;
    bool enabled = true;
};

class TaskSystem;
struct UpdateContext {
    TaskSystem& tasks;
    execution::Executor& jobs;
    TaskId self;
    float delta_seconds;
    std::uint64_t frame;
};

class Task {
public:
    virtual ~Task() = default;
    virtual void update(UpdateContext& context) = 0;
};

class TaskSystem {
public:
    explicit TaskSystem(execution::Executor& jobs);
    ~TaskSystem();
    TaskSystem(const TaskSystem&) = delete;
    TaskSystem& operator=(const TaskSystem&) = delete;

    template<class T, class... Args>
    TaskId create(TaskId parent, std::string name, TaskOptions options, Args&&... args) {
        static_assert(std::is_base_of_v<Task, T>);
        validate_parent(parent);
        auto task = std::allocate_shared<T>(memory::Allocator<T, memory::Domain::Gameplay>{},
                                            std::forward<Args>(args)...);
        return add(parent, std::move(name), options, std::move(task));
    }

    TaskId root() const noexcept;
    TaskId layer(Layer value) const;
    Task* find(TaskId task) const;
    bool alive(TaskId task) const;
    TaskId parent(TaskId task) const;
    std::vector<TaskId> children(TaskId task) const;
    bool request_destroy(TaskId task);
    bool set_enabled(TaskId task, bool enabled);
    bool set_pause_policy(TaskId task, bool update_while_paused);
    bool reparent(TaskId task, TaskId parent);
    void request_destroy_children(TaskId parent);
    void set_paused(bool paused);
    bool paused() const;
    void tick(float delta_seconds);
    void update_children(TaskId parent, float delta_seconds);
    // The scene reset the original performs at 0x801EB624: mark the Scene
    // subtree, walk it away, rebuild the thirteen scene layers. It takes no
    // delta because the original passes 0.0f (0x801EB644 loads it from
    // 0x805F7198); no task runs an update during a reset in any case.
    void reset_scene();

    struct Stats {
        std::uint64_t frames = 0;
        std::uint64_t created = 0;
        std::uint64_t destroyed = 0;
        std::uint64_t updates = 0;
        std::uint64_t paused_updates = 0;
        std::size_t live = 0;
        std::size_t peak_live = 0;
        std::size_t peak_depth = 0;
    };
    Stats stats() const;

private:
    void validate_parent(TaskId parent) const;
    TaskId add(TaskId parent, std::string name, TaskOptions options, std::shared_ptr<Task> task);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
