#pragma once

// One deferred computation that exactly one thread runs, and that any thread can finish or wait for (the decode-ahead's
// per-member job, gekko/prs_ahead.h). A worker pops it and calls run_once(); the thread that needs the result first
// calls finish(): when no worker has started the task it runs it itself (the worker's later run_once() does nothing),
// when a worker is in the middle of it, it waits for it (bounded). The computation is never run twice, and the result is
// the same whichever thread ran it - only which thread paid for it differs.

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <utility>

namespace gekko {

template <class Result>
class ClaimableTask {
public:
    explicit ClaimableTask(std::function<Result()> work) : task_(std::move(work)), result_(task_.get_future().share()) {}
    ClaimableTask(const ClaimableTask&) = delete;
    ClaimableTask& operator=(const ClaimableTask&) = delete;

    // Runs the computation unless another thread claimed it already. True: this call ran it.
    bool run_once() {
        if (claimed_.exchange(1, std::memory_order_acq_rel) != 0) return false;
        task_();
        return true;
    }
    bool ready() const { return result_.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }
    const std::shared_future<Result>& result() const noexcept { return result_; }

    enum class Outcome {
        Ready,     // the result was there already
        Claimed,   // no worker had started it: this thread ran it
        Waited,    // a worker was running it: this thread waited, and it finished within the limit
        TimedOut,  // a worker was running it and did not finish within the limit: no result yet
    };
    Outcome finish(std::chrono::milliseconds limit) {
        if (ready()) return Outcome::Ready;
        if (run_once()) return Outcome::Claimed;
        return result_.wait_for(limit) == std::future_status::ready ? Outcome::Waited : Outcome::TimedOut;
    }

private:
    std::packaged_task<Result()> task_;
    std::shared_future<Result> result_;
    std::atomic<int> claimed_{0};
};

}  // namespace gekko
