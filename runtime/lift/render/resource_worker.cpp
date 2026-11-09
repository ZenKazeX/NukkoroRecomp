#include "render/resource_worker.h"
#include "../../include/gekko/thread_name.h"  // ES-T1 (relative: shadow_render has only runtime/lift)

#include "render/profile.h"

#include <algorithm>
#include <chrono>

namespace shadow::render {

namespace {

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

ResourceWorker::~ResourceWorker() {
    stop();
}

void ResourceWorker::start() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (thread_.joinable())
        return;
    stopping_ = false;
    thread_ = std::thread([this] {
        GEKKO_PROFILE_THREAD("render-worker");
        gekko::name_this_thread("render-resource-worker");  // ES-T1
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queued_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty())
                    return;  // stopping, nothing left
            }
            run_next(~std::uint64_t{0}, true);
        }
    });
}

void ResourceWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stopping_ = true;
    }
    queued_.notify_all();
    if (thread_.joinable())
        thread_.join();
    // Nothing is left behind half made: whatever is still queued runs here.
    while (run_next(~std::uint64_t{0}, false)) {
    }
}

std::uint64_t ResourceWorker::submit(Job job) {
    std::uint64_t ticket = 0;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        ticket = ++submitted_;
        queue_.emplace_back(ticket, std::move(job));
        ++stats_.submitted;
        stats_.peak_queued = std::max<std::uint64_t>(stats_.peak_queued, queue_.size());
    }
    queued_.notify_one();
    return ticket;
}

bool ResourceWorker::run_next(std::uint64_t limit, bool by_worker) {
    std::lock_guard<std::mutex> exec(exec_mutex_);
    std::pair<std::uint64_t, Job> item;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queue_.empty() || queue_.front().first > limit)
            return false;
        item = std::move(queue_.front());
        queue_.pop_front();
    }
    const std::uint64_t start = now_ns();
    {
        GEKKO_PROFILE_ZONE("res/gpu-job");
        item.second();
    }
    const std::uint64_t spent = now_ns() - start;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        completed_.store(item.first, std::memory_order_release);
        stats_.busy_ns += spent;
        if (by_worker)
            ++stats_.run_by_worker;
        else
            ++stats_.run_by_waiters;
    }
    finished_.notify_all();
    return true;
}

void ResourceWorker::wait(std::uint64_t ticket) {
    if (done(ticket))
        return;
    const std::uint64_t start = now_ns();
    for (;;) {
        if (done(ticket))
            break;
        if (run_next(ticket, false))
            continue;
        // Nothing up to the ticket is queued any more: the worker is running the last of them.
        std::unique_lock<std::mutex> lock(queue_mutex_);
        finished_.wait(lock, [this, ticket] { return done(ticket); });
        break;
    }
    std::lock_guard<std::mutex> lock(queue_mutex_);
    ++stats_.waits;
    stats_.wait_ns += now_ns() - start;
}

ResourceWorker::Stats ResourceWorker::stats() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return stats_;
}

}  // namespace shadow::render
