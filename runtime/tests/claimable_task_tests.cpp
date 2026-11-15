// Contract of ClaimableTask (runtime/include/gekko/claimable_task.h), the one-decode rule of the PRS decode-ahead:
//  * every task's computation runs exactly once however many threads race to run or finish it;
//  * the result is the same whichever thread ran it;
//  * a task nobody started is claimed by finish() (and the worker's later run_once() is a no-op), a task a worker is in
//    the middle of is waited for, and a wait that outlasts its limit reports TimedOut without a result;
//  * a negative control: a variant without the claim runs the computation twice (or throws on the second packaged_task
//    call) under the same schedule, and this test catches it.
#include "gekko/claimable_task.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <thread>
#include <vector>

namespace {
using gekko::ClaimableTask;
using Outcome = ClaimableTask<int>::Outcome;
using namespace std::chrono_literals;

// The variant without the claim: finish() runs the computation whenever the result is not there yet, even while a
// worker is in the middle of it.
class ClaimlessTask {
public:
    explicit ClaimlessTask(std::function<int()> work) : work_(std::move(work)), result_(promise_.get_future().share()) {}
    void run() {
        const int value = work_();
        try { promise_.set_value(value); } catch (const std::future_error&) {}
    }
    void finish() {
        if (result_.wait_for(0s) != std::future_status::ready) run();
    }
private:
    std::function<int()> work_;
    std::promise<int> promise_;
    std::shared_future<int> result_;
};
}  // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        std::printf("%-78s %s\n", what, ok ? "ok" : "FAILED");
        failures += !ok;
    };

    // 1. A race: 400 tasks, 4 workers popping a shared queue, the main thread finishing the tasks in a different order.
    {
        constexpr int kTasks = 400;
        std::vector<std::atomic<int>> runs(kTasks);
        std::vector<std::shared_ptr<ClaimableTask<int>>> tasks;
        for (int i = 0; i < kTasks; ++i) {
            runs[i] = 0;
            tasks.push_back(std::make_shared<ClaimableTask<int>>([&runs, i] {
                runs[i].fetch_add(1);
                int x = i;
                for (int k = 0; k < 2000; ++k) x = (x * 1103515245 + 12345) & 0x7FFFFFFF;  // some work
                return x ^ i;
            }));
        }
        std::atomic<int> next{0};
        std::vector<std::thread> workers;
        for (int w = 0; w < 4; ++w)
            workers.emplace_back([&] {
                for (;;) {
                    const int i = next.fetch_add(1);
                    if (i >= kTasks) return;
                    tasks[i]->run_once();
                }
            });
        int ready = 0, claimed = 0, waited = 0, timed_out = 0;
        bool values_ok = true;
        for (int i = kTasks - 1; i >= 0; --i) {  // the opposite order to the workers'
            switch (tasks[i]->finish(10s)) {
                case Outcome::Ready: ++ready; break;
                case Outcome::Claimed: ++claimed; break;
                case Outcome::Waited: ++waited; break;
                case Outcome::TimedOut: ++timed_out; break;
            }
            int x = i;
            for (int k = 0; k < 2000; ++k) x = (x * 1103515245 + 12345) & 0x7FFFFFFF;
            values_ok = values_ok && tasks[i]->result().get() == (x ^ i);
        }
        for (std::thread& t : workers) t.join();
        bool once = true;
        for (int i = 0; i < kTasks; ++i) once = once && runs[i].load() == 1;
        std::printf("  race: %d ready, %d claimed by the finisher, %d waited, %d timed out\n", ready, claimed, waited, timed_out);
        check(once, "every task ran exactly once under the race");
        check(values_ok, "every result equals the computation's value, whichever thread ran it");
        check(timed_out == 0 && ready + claimed + waited == kTasks, "no finish timed out; every outcome accounted for");
    }

    // 2. Nobody started it: finish() runs it; the worker's later run_once() does nothing.
    {
        std::atomic<int> runs{0};
        ClaimableTask<int> task([&] { runs.fetch_add(1); return 42; });
        check(task.finish(1s) == Outcome::Claimed, "an unstarted task is claimed by finish()");
        check(!task.run_once() && runs.load() == 1 && task.result().get() == 42, "the worker's later run_once() is a no-op");
        check(task.finish(1s) == Outcome::Ready, "a finished task reports Ready");
    }

    // 3. A worker is in the middle of it: finish() waits; a limit shorter than the work reports TimedOut.
    for (const bool long_wait : {true, false}) {
        std::atomic<bool> started{false};
        std::atomic<int> runs{0};
        ClaimableTask<int> task([&] {
            started = true;
            runs.fetch_add(1);
            std::this_thread::sleep_for(150ms);
            return 7;
        });
        std::thread worker([&] { task.run_once(); });
        while (!started.load()) std::this_thread::yield();
        const Outcome got = task.finish(long_wait ? 5s : 1ms);
        check(got == (long_wait ? Outcome::Waited : Outcome::TimedOut),
              long_wait ? "a task a worker is running is waited for" : "a wait shorter than the work times out");
        if (!long_wait) check(!task.ready(), "a timed-out task has no result yet");
        worker.join();
        check(runs.load() == 1 && task.result().get() == 7, "the worker's single run produced the result");
    }

    // 4. Negative control: the same in-flight schedule as section 3 without the claim runs the computation twice.
    {
        std::atomic<bool> started{false};
        std::atomic<int> runs{0};
        ClaimlessTask task([&] {
            started = true;
            runs.fetch_add(1);
            std::this_thread::sleep_for(150ms);
            return 7;
        });
        std::thread worker([&] { task.run(); });
        while (!started.load()) std::this_thread::yield();
        task.finish();
        worker.join();
        check(runs.load() == 2, "negative control: without the claim the in-flight task runs twice (and is caught)");
    }

    std::puts(failures == 0 ? "claimable_task_tests: ok" : "claimable_task_tests: FAILED");
    return failures == 0 ? 0 : 1;
}
