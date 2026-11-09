#pragma once

// The renderer's resource worker (ES 2026-10-19): ledger docs/lift-20261006/RECONSTRUCTION_LEDGER.md 2.4 "async texture
// import", R_DESIGN.md 2.6 / 2.10 ("load batches ... go through the copy queue on worker threads").
//
// The game asks for a texture or a mesh and gets its handle at once - allocated on the asking thread, in the order of
// the asks, so a handle's value is what it always was. What MAKING one costs - decoding the texels and building the mip
// chain, converting the vertices, creating the GPU resources, staging the upload - is a job here, run in submission
// order on one worker thread. Measured before (findings-20261019-ES/ES_REPORT.md 2): that work was 18-37 % of every
// stage-load stall on the game thread.
//
// Whoever needs a job done before going on (the renderer recording a packet that names the resource, a synchronous
// create) waits for its ticket and, while it waits, runs the queued jobs up to that ticket itself, in the same order -
// so no wait depends on the worker thread being scheduled. Jobs run one at a time whoever runs them (the execution
// lock), so they may share state that is not thread-safe (the upload stream).
//
// Lock order: the execution lock may be held while a job takes the renderer's table lock; nothing holding the table
// lock ever waits here (submit takes only the queue lock).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace shadow::render {

class ResourceWorker {
public:
    using Job = std::function<void()>;

    ResourceWorker() = default;
    ResourceWorker(const ResourceWorker&) = delete;
    ResourceWorker& operator=(const ResourceWorker&) = delete;
    ~ResourceWorker();

    void start();
    // Runs whatever is still queued (on the calling thread if the worker is gone), then joins the worker.
    void stop();

    // Any thread. Returns the job's ticket: 1, 2, 3, ... in submission order.
    std::uint64_t submit(Job job);
    // Returns once every job up to `ticket` has run; runs queued jobs up to it itself while waiting.
    void wait(std::uint64_t ticket);
    bool done(std::uint64_t ticket) const noexcept { return completed_.load(std::memory_order_acquire) >= ticket; }

    struct Stats {
        std::uint64_t submitted = 0;
        std::uint64_t run_by_worker = 0;
        std::uint64_t run_by_waiters = 0;  // jobs a waiting thread ran itself
        std::uint64_t waits = 0;           // waits that found their ticket not done yet
        std::uint64_t wait_ns = 0;         // wall time in those waits (running jobs included)
        std::uint64_t busy_ns = 0;         // wall time inside jobs, whoever ran them
        std::uint64_t peak_queued = 0;
    };
    Stats stats() const;

private:
    // Under the execution lock: runs the front job if its ticket is <= `limit`. False when there is none.
    bool run_next(std::uint64_t limit, bool by_worker);

    mutable std::mutex queue_mutex_;
    std::condition_variable queued_;    // a job was submitted, or stop
    std::condition_variable finished_;  // a job finished
    std::deque<std::pair<std::uint64_t, Job>> queue_;
    std::uint64_t submitted_ = 0;
    std::atomic<std::uint64_t> completed_{0};
    Stats stats_;
    bool stopping_ = false;
    std::mutex exec_mutex_;  // one job at a time, whoever runs it
    std::thread thread_;
};

}  // namespace shadow::render
