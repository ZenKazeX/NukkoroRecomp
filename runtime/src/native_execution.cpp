#include "../include/native_execution.h"
#include "../include/gekko/thread_name.h"  // ES-T1 (relative: this target has no runtime/include path)

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#if !defined(_M_X64) && !defined(__x86_64__) && !defined(__aarch64__) && !defined(_M_ARM64)
#error Native execution requires a 64-bit target (x86-64 or arm64)
#endif

namespace shadow::execution {

static_assert(sizeof(void*) == 8);

TaskGraph::TaskId TaskGraph::add(std::function<void()> operation, std::vector<TaskId> dependencies) {
    if (!operation) throw std::invalid_argument("empty native task");
    for (const auto& dependency : dependencies) {
        if (dependency.owner != this || dependency.index >= tasks_.size())
            throw std::invalid_argument("dependency must be an earlier task in this graph");
    }
    std::sort(dependencies.begin(), dependencies.end(),
              [](TaskId a, TaskId b) { return a.index < b.index; });
    if (std::adjacent_find(dependencies.begin(), dependencies.end(),
                          [](TaskId a, TaskId b) { return a.index == b.index; }) != dependencies.end())
        throw std::invalid_argument("duplicate native task dependency");
    const TaskId id{this, tasks_.size()};
    tasks_.push_back({std::move(operation), std::move(dependencies)});
    return id;
}

struct Executor::Impl {
    struct Run;
    struct Node {
        Run* run = nullptr;
        Node* next = nullptr;
        std::function<void()> operation;
        std::vector<std::size_t> successors;
        std::size_t prerequisites = 0;
        bool blocked = false;
    };
    struct Run {
        std::vector<Node> nodes;
        std::size_t remaining = 0;
        Result result;
        std::exception_ptr error;
    };

    mutable std::mutex mutex;
    std::condition_variable changed;
    std::once_flag shutdown_once;
    std::vector<std::thread> workers;
    Node* head = nullptr;
    Node* tail = nullptr;
    std::size_t queue_size = 0;
    std::size_t active_runs = 0;
    bool accepting = true;
    bool stopping = false;
    Stats counters;
    static thread_local Impl* executing;
    static thread_local Impl* worker_pool;

    void enqueue(Node& node) noexcept {
        node.next = nullptr;
        if (tail) tail->next = &node;
        else head = &node;
        tail = &node;
        ++queue_size;
        counters.queue_peak = std::max(counters.queue_peak, queue_size);
    }

    Node* take() noexcept {
        Node* node = head;
        head = node->next;
        if (!head) tail = nullptr;
        --queue_size;
        return node;
    }

    void perform(Node& node, bool worker) noexcept {
        auto* previous = executing;
        executing = this;
        std::exception_ptr error;
        if (!node.blocked) {
            try { node.operation(); }
            catch (...) { error = std::current_exception(); }
        }
        executing = previous;
        std::lock_guard lock(mutex);
        Run& run = *node.run;
        if (node.blocked) {
            ++run.result.skipped;
            ++counters.skipped;
        } else {
            ++run.result.executed;
            ++counters.executed;
            if (worker) ++counters.worker_tasks;
            else ++counters.caller_tasks;
        }
        if (error) {
            ++counters.failed;
            if (!run.error) run.error = error;
        }
        bool published = false;
        for (std::size_t successor : node.successors) {
            auto& child = run.nodes[successor];
            child.blocked = child.blocked || node.blocked || bool(error);
            if (--child.prerequisites == 0u) {
                enqueue(child);
                published = true;
            }
        }
        if (--run.remaining == 0u) {
            --active_runs;
            published = true;
        }
        if (published) changed.notify_all();
    }

    void work() {
        gekko::name_this_thread("executor-worker");  // ES-T1
        worker_pool = this;
        for (;;) {
            Node* node;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return stopping || head != nullptr; });
                if (stopping && !head) return;
                node = take();
            }
            perform(*node, true);
        }
    }
};

thread_local Executor::Impl* Executor::Impl::executing = nullptr;
thread_local Executor::Impl* Executor::Impl::worker_pool = nullptr;

Executor::Executor(unsigned requested) : impl_(std::make_unique<Impl>()) {
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const unsigned count = requested ? std::min(requested, hardware)
                                    : (hardware > 2u ? hardware - 2u : 1u);
    impl_->workers.reserve(count);
    try {
        for (unsigned i = 0; i < count; ++i)
            impl_->workers.emplace_back([impl = impl_.get()] { impl->work(); });
    } catch (...) {
        {
            std::lock_guard lock(impl_->mutex);
            impl_->stopping = true;
            impl_->changed.notify_all();
        }
        for (auto& worker : impl_->workers) worker.join();
        throw;
    }
    impl_->counters.workers = count;
}

Executor::~Executor() { shutdown(); }

Executor::Result Executor::execute(const TaskGraph& graph) {
    Impl::Run run;
    run.nodes.resize(graph.tasks_.size());
    run.remaining = run.nodes.size();
    for (std::size_t i = 0; i < run.nodes.size(); ++i) {
        auto& node = run.nodes[i];
        node.run = &run;
        node.operation = graph.tasks_[i].operation;
        node.prerequisites = graph.tasks_[i].dependencies.size();
        for (const auto& dependency : graph.tasks_[i].dependencies)
            run.nodes[dependency.index].successors.push_back(i);
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->accepting) throw std::runtime_error("native executor is shut down");
        ++impl_->counters.graphs;
        if (run.remaining == 0u) return {};
        ++impl_->active_runs;
        for (auto& node : run.nodes)
            if (node.prerequisites == 0u) impl_->enqueue(node);
        impl_->changed.notify_all();
    }
    for (;;) {
        Impl::Node* node;
        {
            std::unique_lock lock(impl_->mutex);
            impl_->changed.wait(lock, [&] { return run.remaining == 0u || impl_->head != nullptr; });
            if (run.remaining == 0u) break;
            node = impl_->take();
        }
        impl_->perform(*node, Impl::worker_pool == impl_.get());
    }
    if (run.error) std::rethrow_exception(run.error);
    return run.result;
}

void Executor::shutdown() {
    if (Impl::executing == impl_.get())
        throw std::logic_error("cannot shut down an executor from its own task");
    std::call_once(impl_->shutdown_once, [&] {
        {
            std::unique_lock lock(impl_->mutex);
            impl_->accepting = false;
            impl_->changed.wait(lock, [&] { return impl_->active_runs == 0u; });
            impl_->stopping = true;
            impl_->changed.notify_all();
        }
        for (auto& worker : impl_->workers) worker.join();
    });
}

Executor::Stats Executor::stats() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->counters;
}

unsigned Executor::worker_count() const noexcept { return impl_->counters.workers; }

}
