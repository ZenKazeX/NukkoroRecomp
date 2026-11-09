#include "../include/native_execution.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using shadow::execution::Executor;
using shadow::execution::TaskGraph;

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void dependencies() {
    Executor executor(3);
    int input = 0, left = 0, right = 0, result = 0;
    TaskGraph graph;
    auto prepare = graph.add([&] { input = 21; });
    auto a = graph.add([&] { left = input * 2; }, {prepare});
    auto b = graph.add([&] { right = input + 1; }, {prepare});
    graph.add([&] { result = left + right; }, {a, b});
    for (unsigned i = 0; i < 200; ++i) {
        input = left = right = result = 0;
        const auto completed = executor.execute(graph);
        require(result == 64 && completed.executed == 4 && completed.skipped == 0,
                "dependency visibility or exactly-once execution failed");
    }
}

void concurrent_workers() {
    Executor executor(3);
    const unsigned participants = executor.worker_count() + 1u;
    std::mutex mutex;
    std::condition_variable ready;
    std::set<std::thread::id> threads;
    unsigned entered = 0;
    TaskGraph graph;
    for (unsigned i = 0; i < participants; ++i) {
        graph.add([&] {
            std::unique_lock lock(mutex);
            threads.insert(std::this_thread::get_id());
            ++entered;
            ready.notify_all();
            if (!ready.wait_for(lock, std::chrono::seconds(5), [&] { return entered == participants; }))
                throw std::runtime_error("native tasks did not execute concurrently");
        });
    }
    executor.execute(graph);
    require(threads.size() == participants, "tasks did not use separate host threads");
    const auto stats = executor.stats();
    require(stats.worker_tasks >= executor.worker_count() && stats.executed == participants,
            "worker execution statistics incorrect");
}

void nested_graphs() {
    Executor executor(1);
    std::atomic<unsigned> calls{0};
    TaskGraph parents;
    for (unsigned i = 0; i < 2; ++i) {
        parents.add([&] {
            TaskGraph children;
            auto prepare = children.add([&] { ++calls; });
            for (unsigned j = 0; j < 16; ++j) children.add([&] { ++calls; }, {prepare});
            executor.execute(children);
        });
    }
    executor.execute(parents);
    require(calls == 34, "nested graphs deadlocked or lost tasks");
}

void failed_dependencies() {
    Executor executor(2);
    std::atomic<unsigned> independent{0}, forbidden{0};
    TaskGraph graph;
    auto failure = graph.add([] { throw std::runtime_error("expected failure"); });
    auto child = graph.add([&] { ++forbidden; }, {failure});
    graph.add([&] { ++forbidden; }, {child});
    graph.add([&] { ++independent; });
    bool threw = false;
    try { executor.execute(graph); } catch (const std::runtime_error&) { threw = true; }
    require(threw && independent == 1 && forbidden == 0, "failed dependencies were executed");
    require(executor.stats().failed == 1 && executor.stats().skipped == 2, "failure was not propagated");
    TaskGraph next;
    next.add([&] { ++independent; });
    executor.execute(next);
    require(independent == 2, "executor cannot recover after a task failure");
}

void simultaneous_submitters() {
    Executor executor(3);
    std::atomic<unsigned> visits{0}, errors{0};
    std::vector<std::thread> submitters;
    for (unsigned i = 0; i < 12; ++i) {
        submitters.emplace_back([&] {
            try {
                TaskGraph graph;
                for (unsigned j = 0; j < 100; ++j) graph.add([&] { ++visits; });
                executor.execute(graph);
            } catch (...) { ++errors; }
        });
    }
    for (auto& thread : submitters) thread.join();
    require(errors == 0 && visits == 1200, "concurrent submissions lost work");
    require(executor.stats().graphs == 12 && executor.stats().executed == 1200, "submission accounting incorrect");
}

void shutdown_drain() {
    Executor executor(2);
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false, release = false;
    std::atomic<bool> completed{false}, error{false};
    TaskGraph graph;
    auto first = graph.add([&] {
        std::unique_lock lock(mutex);
        started = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
    });
    graph.add([&] { completed = true; }, {first});
    std::thread caller([&] {
        try { executor.execute(graph); } catch (...) { error = true; }
    });
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return started; });
    }
    std::thread stopper([&] { executor.shutdown(); });
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    caller.join();
    stopper.join();
    require(completed && !error, "shutdown abandoned an accepted dependency chain");
    bool rejected = false;
    try { executor.execute(graph); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "shutdown executor accepted new work");
    executor.shutdown();
}

void dependency_validation() {
    TaskGraph a, b;
    auto first = a.add([] {});
    auto foreign = b.add([] {});
    unsigned rejected = 0;
    try { a.add([] {}, {foreign}); } catch (const std::invalid_argument&) { ++rejected; }
    try { a.add([] {}, {{&a, 999u}}); } catch (const std::invalid_argument&) { ++rejected; }
    try { a.add([] {}, {first, first}); } catch (const std::invalid_argument&) { ++rejected; }
    try { a.add({}); } catch (const std::invalid_argument&) { ++rejected; }
    require(rejected == 4 && a.size() == 1, "invalid graph mutated the task list");
}

void empty_and_reentrant_shutdown() {
    Executor executor(1);
    TaskGraph empty;
    require(executor.execute(empty).executed == 0, "empty graph did work");
    TaskGraph graph;
    graph.add([&] { executor.shutdown(); });
    bool rejected = false;
    try { executor.execute(graph); } catch (const std::logic_error&) { rejected = true; }
    require(rejected, "task attempted to join its own executor");
}
}

int main() {
    const std::pair<const char*, void(*)()> cases[] = {
        {"dependency ordering and reuse", dependencies},
        {"concurrent native worker execution", concurrent_workers},
        {"nested graphs with one worker", nested_graphs},
        {"failure propagation and recovery", failed_dependencies},
        {"simultaneous submitters", simultaneous_submitters},
        {"shutdown drains dependencies", shutdown_drain},
        {"invalid graph rejection", dependency_validation},
        {"empty graph and reentrant shutdown", empty_and_reentrant_shutdown},
    };
    unsigned failures = 0;
    for (const auto& test : cases) {
        try { test.second(); std::printf("PASS %s\n", test.first); }
        catch (const std::exception& e) { ++failures; std::printf("FAIL %s: %s\n", test.first, e.what()); }
    }
    return failures == 0 ? 0 : 1;
}
