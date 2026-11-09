#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace shadow::execution {

class TaskGraph {
public:
    struct TaskId {
        const TaskGraph* owner = nullptr;
        std::size_t index = 0;
    };

    TaskGraph() = default;
    TaskGraph(const TaskGraph&) = delete;
    TaskGraph& operator=(const TaskGraph&) = delete;

    TaskId add(std::function<void()> operation, std::vector<TaskId> dependencies = {});
    std::size_t size() const noexcept { return tasks_.size(); }

private:
    friend class Executor;
    struct Task {
        std::function<void()> operation;
        std::vector<TaskId> dependencies;
    };
    std::vector<Task> tasks_;
};

class Executor {
public:
    struct Result {
        std::size_t executed = 0;
        std::size_t skipped = 0;
    };

    struct Stats {
        std::uint64_t graphs = 0;
        std::uint64_t executed = 0;
        std::uint64_t skipped = 0;
        std::uint64_t failed = 0;
        std::uint64_t worker_tasks = 0;
        std::uint64_t caller_tasks = 0;
        std::size_t queue_peak = 0;
        unsigned workers = 0;
    };

    explicit Executor(unsigned workers = 0);
    ~Executor();
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    Result execute(const TaskGraph& graph);
    void shutdown();
    Stats stats() const;
    unsigned worker_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
