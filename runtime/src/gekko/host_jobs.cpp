#include "gekko/host_jobs.h"
#include "gekko/thread_name.h"  // ES-T1
#include "gekko/profiling.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace gekko {
namespace {

thread_local bool g_host_job_worker = false;
// Published only after construction. Reporting an unused pool must stay read-only.
std::atomic<HostJobSystem*> g_active_job_system{nullptr};

// Zone name per job site. Distinct from job_site_name only by the subsystem
// prefix the profiler groups on, and kept as literals so the fork path never
// formats a string for a name it emits thousands of times a second.
GEKKO_PROFILE_ONLY(const char* job_site_zone_name(JobSite site) noexcept {
    switch (site) {
        case JobSite::VertexDecode: return "host/job-vertex-decode";
        case JobSite::TextureDecode: return "host/job-texture-decode";
        case JobSite::ClipTriangles: return "host/job-clip-triangles";
        case JobSite::EfbTileWrite: return "host/job-efb-tile-write";
        case JobSite::XfbResolve: return "host/job-xfb-resolve";
        case JobSite::TextureUpload: return "host/job-texture-upload";
        case JobSite::PresentStage: return "host/job-present-stage";
        case JobSite::PresentScale: return "host/job-present-scale";
        case JobSite::SoftwareRaster: return "host/job-software-raster";
        default: return "host/job-unknown";
    }
})

// Lifetime total of WorkerStats::futile_wakes as of the previous fork, so the
// plot below can show the DELTA rather than a monotone sum. Read and written
// only while a profiler is attached, and only to produce a plot point: two
// threads forking at once cost one point's accuracy and nothing else.
GEKKO_PROFILE_ONLY(std::atomic<std::uint64_t> g_futile_wake_mark{0};)

// Wake policy switch (HostJobSystem::set_notify_one). A namespace-scope
// atomic rather than an Impl member so the CLI can set it before the pool is
// constructed. Read once per fork with relaxed order: a flip mid-run only
// changes which notify call the next fork issues.
std::atomic<bool> g_notify_one{true};

// One fork/join barrier. Lives on the STACK of the parallel_chunks call that
// opened it.
// Evidence: D03 D13 / D12 F3 - 247k-265k make_shared<JobGroup> per 31 s
// windowed run, one per group, freed by whichever thread dropped the last
// reference.
// Native implementation: the caller joins (remaining == 0) before
// parallel_chunks returns, so its frame outlives every queued chunk's use of
// the group and no reference count is needed. The one hazard of a stack
// barrier is the LAST worker touching the group after the caller has been
// released: finish_job therefore does the counter transition AND the notify
// while holding wait_mutex, and touches nothing after the unlock. The caller
// cannot pass its own wait_mutex acquisition (the pre-wait check holds it;
// wait_for re-acquires it before returning) until that unlock has happened,
// so every worker access to the group happens-before the caller's return.
// [thread.condition.condvar] allows destroying a condition_variable once all
// waiters have been notified, even if they have not yet re-acquired the lock.
// Guest-invisible: this is host memory management around an unchanged
// barrier; the join and the in-order commit after it are exactly as before.
struct JobGroup {
    std::atomic<unsigned> remaining{0};
    std::mutex wait_mutex;
    std::condition_variable wait_condition;
    std::mutex exception_mutex;
    std::exception_ptr exception;
};

// A queued chunk. Trivially copyable; both pointers refer to objects on the
// opening caller's stack (see JobGroup for why that is sound).
struct QueuedJob {
    int begin = 0;
    int end = 0;
    const std::function<void(int, int)>* operation = nullptr;
    JobGroup* group = nullptr;
};

void remember_exception(JobGroup* group) noexcept {
    std::lock_guard<std::mutex> lock(group->exception_mutex);
    if (group->exception == nullptr) {
        group->exception = std::current_exception();
    }
}

void finish_job(JobGroup* group) noexcept {
    // The counter transition and condition-variable notification must share
    // the waiter mutex.  Updating the atomic without this lock leaves a lost
    // wake-up window between the caller's remaining check and wait_for: the
    // last worker can notify before the caller is actually asleep, causing a
    // completed group to sit on the one-second diagnostic timeout.  Holding
    // wait_mutex closes that window without changing the strict join or the
    // order in which the caller commits guest-visible results.
    //
    // The notify is issued INSIDE the lock as well: the group is on the
    // caller's stack and the unlock must be this worker's last touch of it
    // (see JobGroup). notify_all rather than notify_one only because the
    // group condvar has exactly one waiter either way.
    std::lock_guard<std::mutex> lock(group->wait_mutex);
    if (group->remaining.fetch_sub(1u, std::memory_order_acq_rel) == 1u) {
        group->wait_condition.notify_all();
    }
}

// Persistent FIFO with one waiting slot per successfully created worker.
// A queue slot represents a worker's next partition. Once every worker has
// queued work, the opening caller executes excess partitions itself. No
// allocation, queue copying, or admission wait is needed on the fork path.
struct JobQueue {
    std::vector<QueuedJob> slots;
    std::size_t head = 0;
    std::size_t count = 0;
    std::size_t peak = 0;
    std::atomic<std::uint64_t> capacity_mirror{0};
    std::atomic<std::uint64_t> peak_mirror{0};

    bool empty() const noexcept { return count == 0; }
    std::size_t available() const noexcept { return slots.size() - count; }

    // Called once during construction, before the pool is published.
    void initialize(std::size_t workers) {
        slots.resize(workers);
        capacity_mirror.store(slots.capacity(), std::memory_order_relaxed);
    }
    void push_back(const QueuedJob& job) noexcept {
        slots[(head + count) % slots.size()] = job;
        ++count;
        if (count > peak) {
            peak = count;
            peak_mirror.store(peak, std::memory_order_relaxed);
        }
    }
    QueuedJob pop_front() noexcept {
        const QueuedJob job = slots[head];
        head = (head + 1u) % slots.size();
        --count;
        return job;
    }
};

// Per-worker counters on their own cache line. Eight workers bumping one
// shared atomic per chunk (1.63M chunks per run) bounce that line between
// cores for nothing; report() sums the lines instead. The summed values keep
// their old meaning (worker_chunks, items).
struct alignas(64) WorkerStats {
    std::atomic<std::uint64_t> chunks{0};
    std::atomic<std::uint64_t> items{0};
    std::atomic<std::uint64_t> wakes{0};
    std::atomic<std::uint64_t> futile_wakes{0};
};

}  // namespace

struct HostJobSystem::Impl {
    std::mutex queue_mutex;
    std::condition_variable queue_condition;
    // Per-caller barrier accounting. Written only by the thread that opened the
    // group, after it has already joined, so no lock is needed.
    std::atomic<std::uint64_t> site_groups[static_cast<unsigned>(JobSite::Count)]{};
    std::atomic<std::uint64_t> site_items[static_cast<unsigned>(JobSite::Count)]{};
    std::atomic<std::uint64_t> site_chunks[static_cast<unsigned>(JobSite::Count)]{};
    std::atomic<std::uint64_t> site_wait_ns[static_cast<unsigned>(JobSite::Count)]{};
    std::atomic<std::uint64_t> site_longest_ns[static_cast<unsigned>(JobSite::Count)]{};
    std::atomic<std::uint64_t> site_fork_ns[static_cast<unsigned>(JobSite::Count)]{};
    std::atomic<std::uint64_t> site_longest_fork_ns[static_cast<unsigned>(JobSite::Count)]{};
    JobQueue queue;                                // guarded by queue_mutex
    std::vector<std::thread> workers;
    std::unique_ptr<WorkerStats[]> worker_stats;   // one per worker, sized before spawn
    bool stopping = false;                         // guarded by queue_mutex

    std::atomic<std::uint64_t> parallel_groups{0};
    std::atomic<std::uint64_t> chunks{0};
    std::atomic<std::uint64_t> caller_chunks{0};
    // Items of the chunks the opening callers ran themselves; the workers'
    // share lives in WorkerStats and report() adds the two.
    std::atomic<std::uint64_t> caller_items{0};
    std::atomic<std::uint64_t> notify_calls{0};
    std::atomic<std::uint64_t> caller_help_chunks{0};
    unsigned hardware_threads = 1;
    unsigned requested_workers = 0;

    // Thread ownership: runs on pool worker `worker_index` only; `stats` is
    // that worker's private line and report() only ever reads it.
    void worker_loop(unsigned worker_index) {
        // One profiler lane per worker rather than eight lanes sharing one
        // name: which worker ran a chunk, and which ones sat idle, is the whole
        // reason to look at the pool per thread. The pool is built once in the
        // constructor and never resized, so the name is stable for the run.
        // SetThreadName copies, so a stack buffer is enough.
        GEKKO_PROFILE_ONLY(char worker_name[32];
                           std::snprintf(worker_name, sizeof(worker_name),
                                         "host-job-worker-%u", worker_index);
                           GEKKO_PROFILE_THREAD(worker_name);)
        {
            char os_name[32];  // ES-T1
            std::snprintf(os_name, sizeof(os_name), "host-job-worker-%u", worker_index);
            name_this_thread(os_name);
        }
        g_host_job_worker = true;
        WorkerStats& stats = worker_stats[worker_index];
        for (;;) {
            QueuedJob job;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                // Same predicate the old wait(lock, pred) used: park while
                // there is nothing to do and nobody is stopping. Spelled out so
                // each return from the wait is counted, and counted as futile
                // when the queue is still empty.
                // Evidence: D09 F1 - notify_all readied every parked worker for
                // every group, ~3.7 futile wakes per group convoying on this
                // mutex (estimate); these counters turn that into a number.
                while (!stopping && queue.empty()) {
                    queue_condition.wait(lock);
                    stats.wakes.fetch_add(1u, std::memory_order_relaxed);
                    if (!stopping && queue.empty()) {
                        stats.futile_wakes.fetch_add(1u, std::memory_order_relaxed);
                    }
                }
                if (stopping && queue.empty()) {
                    break;
                }
                job = queue.pop_front();
            }
            try {
                // The heaviest site in this subsystem: 1.63M chunks per 31 s
                // legacy run, ~52.6k/s across the pool. The item count says
                // whether a slow chunk was a big partition or a slow one.
                GEKKO_PROFILE_ZONE("host/job-chunk");
                GEKKO_PROFILE_VALUE(job.end - job.begin);
                (*job.operation)(job.begin, job.end);
            } catch (...) {
                remember_exception(job.group);
            }
            stats.chunks.fetch_add(1u, std::memory_order_relaxed);
            stats.items.fetch_add(static_cast<std::uint64_t>(job.end - job.begin),
                                  std::memory_order_relaxed);
            // Last touch of the caller's frame: once finish_job returns the
            // group and the operation may already be gone (see JobGroup).
            finish_job(job.group);
        }
        g_host_job_worker = false;
    }
};

HostJobSystem& HostJobSystem::instance() {
    static HostJobSystem jobs;
    return jobs;
}

// Worker target, settable once before the pool is first touched. Zero means
// "derive from the machine".
std::atomic<unsigned> g_worker_target{0u};

void set_host_job_worker_target(unsigned workers) {
    g_worker_target.store(workers, std::memory_order_relaxed);
}

void set_host_job_notify_one(bool enabled) noexcept {
    g_notify_one.store(enabled, std::memory_order_relaxed);
}

void HostJobSystem::set_notify_one(bool enabled) noexcept {
    g_notify_one.store(enabled, std::memory_order_relaxed);
}

bool HostJobSystem::notify_one_enabled() const noexcept {
    return g_notify_one.load(std::memory_order_relaxed);
}

HostJobSystem::HostJobSystem() {
    // Plot shapes, declared once on the only path that constructs the pool. All
    // three are counts, so they are staircases: a queue depth holds its value
    // between forks and interpolating would draw a depth that never existed.
    GEKKO_PROFILE_PLOT_CONFIG("host/job queue depth", GEKKO_PLOT_NUMBER, true, true, 0x4FB0FF);
    GEKKO_PROFILE_PLOT_CONFIG("host/job caller-run chunks", GEKKO_PLOT_NUMBER, true, true,
                              0xFF8040);
    GEKKO_PROFILE_PLOT_CONFIG("host/job futile wakes", GEKKO_PLOT_NUMBER, true, true, 0xFFD040);
    auto owned = std::make_unique<Impl>();
    impl_ = owned.get();
    const unsigned hardware = std::max(std::thread::hardware_concurrency(), 1u);
    // This used to be a hard cap of four background workers, on the reasoning
    // that a 480-line GX copy does not need a worker per logical processor.
    // That is true of one 480-line copy and false of the pool as a whole: the
    // host also parallelises the XFB resolve, the present staging and (soon)
    // render-command preparation, and a 32-thread part was running five of them.
    // Reserve two threads - the game thread, which is the serial guest
    // instruction stream, and the UI/present thread - and let everything else
    // work. Per-job chunking already adapts, since host_parallel_for takes
    // min(available, span / grain) participants, so a small job does not
    // suddenly fan out to thirty threads.
    //
    // The cap below is MEASURED, not assumed. With the presentation upscale
    // moved onto the GPU, the host-side parallel work left in a frame is the
    // XFB resolve and present staging - small-grained jobs whose wake-and-join
    // overhead lands on the game thread's critical path. Sweeping the pool on
    // the Westopolis savestate at 1920x1080:
    //
    //     workers  4 -> 60.32 FPS, 1.00 cores busy
    //     workers  8 -> 60.34 FPS, 1.10 cores
    //     workers 16 -> 60.08 FPS, 1.34 cores
    //     workers 30 -> 57.88 FPS, 1.81 cores
    //
    // More threads strictly cost FPS and CPU here. Revisit this cap when there
    // is genuinely parallel host work to spend them on - render-command
    // preparation is the obvious candidate - and use --job-workers to re-sweep
    // rather than raising it on the assumption that more cores must be better.
    const unsigned requested = g_worker_target.load(std::memory_order_relaxed);
    const unsigned derived = hardware > 3u ? hardware - 2u : 1u;
    const unsigned background_workers =
        requested != 0u ? std::min(requested, hardware) : std::min(derived, 8u);
    impl_->hardware_threads = hardware;
    impl_->requested_workers = requested;
    impl_->workers.reserve(background_workers);
    // One counter line per worker, allocated once before any worker exists.
    impl_->worker_stats.reset(new WorkerStats[std::max(background_workers, 1u)]);
    impl_->queue.initialize(background_workers);
    for (unsigned index = 0; index < background_workers; ++index) {
        try {
            impl_->workers.emplace_back([this, index] { impl_->worker_loop(index); });
        } catch (...) {
            break;
        }
    }
    // Thread creation can fail; admission follows actual service capacity.
    // Shrinking a vector of trivial jobs cannot allocate.
    {
        std::lock_guard<std::mutex> lock(impl_->queue_mutex);
        impl_->queue.initialize(impl_->workers.size());
    }
    owned.release();
    g_active_job_system.store(this, std::memory_order_release);
}

HostJobSystem::~HostJobSystem() {
    g_active_job_system.store(nullptr, std::memory_order_release);
    if (impl_ == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(impl_->queue_mutex);
        impl_->stopping = true;
    }
    impl_->queue_condition.notify_all();
    for (std::thread& worker : impl_->workers) {
        if (worker.joinable()) worker.join();
    }
    delete impl_;
    impl_ = nullptr;
}

unsigned HostJobSystem::logical_workers() const noexcept {
    return impl_ != nullptr ? static_cast<unsigned>(impl_->workers.size()) + 1u : 1u;
}

HostJobReport host_job_report() noexcept {
    if (const auto* jobs = g_active_job_system.load(std::memory_order_acquire)) return jobs->report();
    HostJobReport result;
    result.hardware_threads = std::max(std::thread::hardware_concurrency(), 1u);
    result.requested_workers = g_worker_target.load(std::memory_order_relaxed);
    result.notify_one = g_notify_one.load(std::memory_order_relaxed);
    return result;
}

HostJobReport HostJobSystem::report() const noexcept {
    HostJobReport result;
    if (impl_ == nullptr) return result;
    result.parallel_groups = impl_->parallel_groups.load(std::memory_order_relaxed);
    result.chunks = impl_->chunks.load(std::memory_order_relaxed);
    result.caller_chunks = impl_->caller_chunks.load(std::memory_order_relaxed);
    result.items = impl_->caller_items.load(std::memory_order_relaxed);
    result.notify_calls = impl_->notify_calls.load(std::memory_order_relaxed);
    result.notify_one = g_notify_one.load(std::memory_order_relaxed);
    result.caller_help_chunks = impl_->caller_help_chunks.load(std::memory_order_relaxed);
    result.hardware_threads = impl_->hardware_threads;
    result.requested_workers = impl_->requested_workers;
    result.queue_capacity = impl_->queue.capacity_mirror.load(std::memory_order_relaxed);
    result.queue_peak = impl_->queue.peak_mirror.load(std::memory_order_relaxed);
    result.logical_workers = logical_workers();
    // worker_chunks and items keep their historical meaning: the per-worker
    // lines are summed here instead of being bumped on one shared atomic.
    const std::size_t worker_count = impl_->workers.size();
    for (std::size_t index = 0; index < worker_count; ++index) {
        const WorkerStats& stats = impl_->worker_stats[index];
        result.worker_chunks += stats.chunks.load(std::memory_order_relaxed);
        result.items += stats.items.load(std::memory_order_relaxed);
        result.worker_wakes += stats.wakes.load(std::memory_order_relaxed);
        result.futile_wakes += stats.futile_wakes.load(std::memory_order_relaxed);
    }
    for (unsigned index = 0; index < static_cast<unsigned>(JobSite::Count); ++index) {
        result.sites[index].groups = impl_->site_groups[index].load(std::memory_order_relaxed);
        result.sites[index].items = impl_->site_items[index].load(std::memory_order_relaxed);
        result.sites[index].chunks = impl_->site_chunks[index].load(std::memory_order_relaxed);
        result.sites[index].wait_ns = impl_->site_wait_ns[index].load(std::memory_order_relaxed);
        result.sites[index].longest_wait_ns =
            impl_->site_longest_ns[index].load(std::memory_order_relaxed);
        result.sites[index].fork_ns = impl_->site_fork_ns[index].load(std::memory_order_relaxed);
        result.sites[index].longest_fork_ns =
            impl_->site_longest_fork_ns[index].load(std::memory_order_relaxed);
        result.total_wait_ns += result.sites[index].wait_ns;
        result.total_fork_ns += result.sites[index].fork_ns;
    }
    return result;
}

const char* job_site_name(JobSite site) noexcept {
    switch (site) {
        case JobSite::VertexDecode: return "vertex-decode";
        case JobSite::TextureDecode: return "texture-decode";
        case JobSite::ClipTriangles: return "clip-triangles";
        case JobSite::EfbTileWrite: return "efb-tile-write";
        case JobSite::XfbResolve: return "xfb-resolve";
        case JobSite::TextureUpload: return "texture-upload";
        case JobSite::PresentStage: return "present-stage";
        case JobSite::PresentScale: return "present-scale";
        case JobSite::SoftwareRaster: return "software-raster";
        default: return "unknown";
    }
}

void HostJobSystem::parallel_chunks(JobSite site, int begin, int end, int minimum_parallel_span,
                                    const std::function<void(int, int)>& chunk) {
    if (end <= begin) return;
    const int span = end - begin;
    const int grain = std::max(minimum_parallel_span, 1);
    const unsigned available = logical_workers();
    if (impl_ == nullptr || span / grain < 2 || available <= 1u ||
        g_host_job_worker) {
        chunk(begin, end);
        return;
    }

    // The fork timer opens here: everything from the range arithmetic through
    // the queue lock to the last worker notify is what opening the group costs
    // the caller's thread (D09 F2: never measured before, join_open was
    // discarded).
    const auto fork_open = std::chrono::steady_clock::now();

    // Chunk boundaries: UNCHANGED. participant_count and chunk_size are the
    // formulas they always were, and chunk k is [begin + k*chunk_size,
    // min(that + chunk_size, end)) - exactly what the old loop pushed into a
    // heap vector (D12 F3), now computed per chunk instead of materialised,
    // with no per-fork range vector. Guest-invisible: every item
    // writes only its own output partition and the caller commits in original
    // order after the join, so chunking cannot change any result.
    const unsigned participant_count =
        std::min<unsigned>(available, static_cast<unsigned>(span / grain));
    const int chunk_size =
        (span + static_cast<int>(participant_count) - 1) /
        static_cast<int>(participant_count);
    // ceil(span / chunk_size) chunks, <= participant_count <= available.
    const unsigned range_count = static_cast<unsigned>((span - 1) / chunk_size + 1);
    if (range_count <= 1u) {
        chunk(begin, end);
        return;
    }
    // The site zone opens HERE, below BOTH early-outs, and not at the top of
    // the function: above, it fired once per CALL - the legacy per-primitive-
    // batch rate - and this is the dynamic-name variant, which transfers the
    // name with every event. Below, it fires once per group that actually
    // forks, which is what the name is describing. The span says how many items
    // the group covered; site_items only has the lifetime total.
    GEKKO_PROFILE_ZONE_DYNAMIC(job_site_zone_name(site));
    GEKKO_PROFILE_VALUE(span);
    const auto range_begin = [&](unsigned index) noexcept -> int {
        return begin + static_cast<int>(index) * chunk_size;
    };
    const auto range_end = [&](unsigned index) noexcept -> int {
        return std::min(range_begin(index) + chunk_size, end);
    };

    // The barrier lives on this frame; see JobGroup for the lifetime argument.
    JobGroup group;
    unsigned queued_count = 0;
    const unsigned site_index = static_cast<unsigned>(site);
    GEKKO_PROFILE_ONLY(std::size_t queue_depth = 0;)
    {
        // host/job-fork covers admission and the wake: the queue lock, the
        // pushes and every notify. It cannot start at fork_open above, because
        // the site zone only opens below the early-outs and Tracy zones nest
        // strictly per thread - a zone that started at fork_open would straddle
        // the site zone and corrupt the lane. The range arithmetic between
        // fork_open and here, and the clock read that closes the fork timer
        // below, are therefore inside site_fork_ns but outside this zone.
        GEKKO_PROFILE_ZONE("host/job-fork");
        {
            std::lock_guard<std::mutex> lock(impl_->queue_mutex);
            queued_count = std::min<unsigned>(
                range_count - 1u, static_cast<unsigned>(impl_->queue.available()));
            group.remaining.store(queued_count, std::memory_order_relaxed);
            for (unsigned index = 1; index <= queued_count; ++index) {
                impl_->queue.push_back(
                    QueuedJob{range_begin(index), range_end(index), &chunk, &group});
            }
            // Depth has to be read under the lock, but it is PLOTTED below it:
            // this mutex is the one place the forking caller and every worker
            // collide, and a Tracy queue write inside it would lengthen the
            // hottest critical section in the file for nothing.
            GEKKO_PROFILE_ONLY(queue_depth = impl_->queue.count;)
        }
        // The queue has exactly one slot per worker, so depth against capacity
        // says directly whether admission is saturating; queue_peak is only a
        // lifetime high-water mark.
        GEKKO_PROFILE_PLOT("host/job queue depth", static_cast<double>(queue_depth));
        // How many workers this fork was trying to wake.
        GEKKO_PROFILE_VALUE(queued_count);
        impl_->parallel_groups.fetch_add(1u, std::memory_order_relaxed);
        impl_->chunks.fetch_add(static_cast<std::uint64_t>(range_count),
                                std::memory_order_relaxed);
        // Wake policy (D09 F1). ON: one notify_one per queued chunk readies at
        // most queued_count parked workers. OFF: the historical notify_all readies
        // every parked worker and the surplus convoy on queue_mutex only to find
        // it empty. Both run outside the queue lock so a woken worker never blocks
        // on a lock the caller still holds. Neither loses a wake: a worker parks
        // only after seeing the queue empty under the same mutex the pushes held,
        // and a worker that finishes a chunk re-checks the queue before parking,
        // so a job whose notify found nobody parked is taken by a running worker.
        // Guest-invisible: which worker runs which chunk is host scheduling; the
        // barrier and the in-order commit after it are unchanged.
        if (g_notify_one.load(std::memory_order_relaxed)) {
            for (unsigned index = 0; index < queued_count; ++index) {
                impl_->queue_condition.notify_one();
            }
            impl_->notify_calls.fetch_add(static_cast<std::uint64_t>(queued_count),
                                          std::memory_order_relaxed);
        } else if (queued_count != 0u) {
            impl_->queue_condition.notify_all();
            impl_->notify_calls.fetch_add(1u, std::memory_order_relaxed);
        }
    }
    const auto fork_done = std::chrono::steady_clock::now();

    // D09 F1's "~3.7 futile wakes per group" is an ESTIMATE; the per-worker
    // counters measure it exactly, but only as a lifetime total. The delta per
    // fork puts the convoy on a time axis: is the wake policy readying workers
    // that find nothing? Eight foreign cache lines are read to produce it, so
    // it runs only while a profiler is attached, and it writes nothing the
    // production path can see.
    if (GEKKO_PROFILE_CONNECTED()) {
        GEKKO_PROFILE_ONLY(
            std::uint64_t futile_total = 0;
            const std::size_t futile_workers = impl_->workers.size();
            for (std::size_t index = 0; index < futile_workers; ++index) {
                futile_total += impl_->worker_stats[index].futile_wakes.load(
                    std::memory_order_relaxed);
            }
            const std::uint64_t futile_previous =
                g_futile_wake_mark.exchange(futile_total, std::memory_order_relaxed);
            GEKKO_PROFILE_PLOT("host/job futile wakes",
                               static_cast<double>(futile_total > futile_previous
                                                       ? futile_total - futile_previous
                                                       : 0u));)
    }

    // Execute exactly the original partitions. Catch each failure separately:
    // an exception must not abandon later caller-owned or queued partitions.
    const auto run_caller_chunk = [&](unsigned index) {
        try {
            chunk(range_begin(index), range_end(index));
        } catch (...) {
            remember_exception(&group);
        }
        impl_->caller_chunks.fetch_add(1u, std::memory_order_relaxed);
        impl_->caller_items.fetch_add(
            static_cast<std::uint64_t>(range_end(index) - range_begin(index)),
            std::memory_order_relaxed);
    };
    run_caller_chunk(0u);
    for (unsigned index = queued_count + 1u; index < range_count; ++index)
        run_caller_chunk(index);
    impl_->caller_help_chunks.fetch_add(range_count - 1u - queued_count,
                                        std::memory_order_relaxed);
    // This subsystem's admission REFUSAL: partitions the pool would not take,
    // which the opening caller then ran on its own critical path. On a timeline
    // it separates a pool-capacity problem from a wake-latency problem without
    // another experiment.
    GEKKO_PROFILE_PLOT("host/job caller-run chunks",
                       static_cast<double>(range_count - 1u - queued_count));

    const auto join_enter = std::chrono::steady_clock::now();
    {
        // The strict barrier, on whichever thread opened the group.
        // site_wait_ns/site_longest_ns already total it per site, but with no
        // time axis, so a barrier that blocked the game thread could not be
        // lined up against the pacer or the presenter. The 5 s diagnostic below
        // re-takes queue_mutex inside this zone: a group that reaches it shows
        // as a multi-second bar, which is the intended reading.
        GEKKO_PROFILE_ZONE("host/job-join");
        GEKKO_PROFILE_VALUE(queued_count);
        std::unique_lock<std::mutex> lock(group.wait_mutex);
        unsigned seconds_waited = 0;
        while (group.remaining.load(std::memory_order_acquire) != 0u) {
            if (group.wait_condition.wait_for(
                    lock, std::chrono::seconds(1), [&] {
                        return group.remaining.load(std::memory_order_acquire) == 0u;
                    })) {
                break;
            }
            ++seconds_waited;
            if ((seconds_waited % 5u) == 0u) {
                std::size_t queued = 0;
                {
                    std::lock_guard<std::mutex> queue_lock(impl_->queue_mutex);
                    queued = impl_->queue.count;
                }
                std::fprintf(stderr,
                             "[host-jobs] group still waiting after %u s: %u chunks remaining, %zu globally queued\n",
                             seconds_waited,
                             group.remaining.load(std::memory_order_acquire), queued);
                std::fflush(stderr);
            }
        }
    }
    // Attribute the barrier. wait_ns is what the CALLER lost sitting in the
    // join after finishing its own chunk - the serialization the fork/join
    // shape imposes on whichever thread opened the group. fork_ns is the
    // other half: what the caller spent opening it.
    if (site_index < static_cast<unsigned>(JobSite::Count)) {
        const auto join_done = std::chrono::steady_clock::now();
        const std::uint64_t waited = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(join_done - join_enter)
                .count());
        const std::uint64_t forked = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(fork_done - fork_open)
                .count());
        impl_->site_groups[site_index].fetch_add(1u, std::memory_order_relaxed);
        impl_->site_items[site_index].fetch_add(static_cast<std::uint64_t>(span),
                                                std::memory_order_relaxed);
        impl_->site_chunks[site_index].fetch_add(
            static_cast<std::uint64_t>(range_count), std::memory_order_relaxed);
        impl_->site_wait_ns[site_index].fetch_add(waited, std::memory_order_relaxed);
        std::uint64_t longest =
            impl_->site_longest_ns[site_index].load(std::memory_order_relaxed);
        while (waited > longest &&
               !impl_->site_longest_ns[site_index].compare_exchange_weak(
                   longest, waited, std::memory_order_relaxed)) {
        }
        impl_->site_fork_ns[site_index].fetch_add(forked, std::memory_order_relaxed);
        std::uint64_t longest_fork =
            impl_->site_longest_fork_ns[site_index].load(std::memory_order_relaxed);
        while (forked > longest_fork &&
               !impl_->site_longest_fork_ns[site_index].compare_exchange_weak(
                   longest_fork, forked, std::memory_order_relaxed)) {
        }
    }
    // Every queued chunk has run finish_job and released wait_mutex, so the
    // group's exception slot is quiescent and the frame may go out of scope.
    if (group.exception != nullptr) {
        std::rethrow_exception(group.exception);
    }
}

}  // namespace gekko
