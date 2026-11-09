#pragma once

// Host-only jobs for resource preparation and independent row/tile work.
//
// The guest CPU, device ticks and GX command stream remain on their original
// ordered execution thread. A parallel group is a strict barrier: each item
// may write only its own output partition, all workers join here, and the
// caller performs any guest-visible commit afterward in original order.

#include <cstdint>
#include <functional>
#include <utility>

namespace gekko {

// Which caller opened a parallel group. Every fork/join in the runtime is
// attributed to one of these so the barriers can be ranked by how long they
// actually block, rather than reasoned about. The caller thread runs the first
// chunk itself and then waits for the rest, so a group is a genuine barrier on
// whichever thread opened it - usually the game thread.
enum class JobSite : unsigned {
    Unknown,
    VertexDecode,      // Renderer::draw_primitive vertex loop
    TextureDecode,     // Renderer::decode_map
    ClipTriangles,     // Renderer::draw_primitive, per primitive
    EfbTileWrite,      // write_efb_*_tiled, per EFB-to-texture copy
    XfbResolve,        // Renderer::resolve_xfb, per displayed frame
    TextureUpload,     // PlumeGxRasterizer index/palette staging
    PresentStage,      // PlumePresenter 1:1 staging
    PresentScale,      // scale_rgba_bilinear (CPU resampler)
    SoftwareRaster,    // Renderer::raster_triangle row jobs
    Count
};

const char* job_site_name(JobSite site) noexcept;

struct JobSiteReport {
    std::uint64_t groups = 0;
    std::uint64_t items = 0;
    std::uint64_t chunks = 0;
    std::uint64_t wait_ns = 0;      // time the CALLER sat in the join
    std::uint64_t longest_wait_ns = 0;
    // Time the CALLER spent opening the group: from the decision to fan out
    // (before the range arithmetic and the queue lock) to the return of the
    // last worker notify. This is the fork half of the fork/join cost; wait_ns
    // is the join half. Evidence: D09 F2 measured neither (join_open was
    // discarded), so the F1 estimate rested on generic wake costs.
    std::uint64_t fork_ns = 0;
    std::uint64_t longest_fork_ns = 0;
};

struct HostJobReport {
    JobSiteReport sites[static_cast<unsigned>(JobSite::Count)]{};
    std::uint64_t total_wait_ns = 0;
    std::uint64_t total_fork_ns = 0;
    std::uint64_t parallel_groups = 0;
    std::uint64_t chunks = 0;
    std::uint64_t worker_chunks = 0;
    std::uint64_t caller_chunks = 0;
    std::uint64_t items = 0;
    // Wake accounting for the worker condition variable (D09 F1).
    //   worker_wakes  - every return from the queue wait on a pool worker
    //   futile_wakes  - of those, the worker found the queue empty and parked
    //                   again (a wake that did no work)
    //   notify_calls  - notify_one/notify_all invocations issued by callers
    //                   (== queued chunks when notify_one is on, == groups
    //                   when it is off), so the A/B can see which arm ran
    std::uint64_t worker_wakes = 0;
    std::uint64_t futile_wakes = 0;
    std::uint64_t notify_calls = 0;
    // One persistent waiting slot per actual worker; excess partitions run
    // on their opening caller. No fork-time queue growth or admission wait.
    std::uint64_t queue_capacity = 0;
    std::uint64_t queue_peak = 0;
    std::uint64_t caller_help_chunks = 0;
    unsigned hardware_threads = 1;
    unsigned requested_workers = 0;
    unsigned logical_workers = 1;
    bool notify_one = true;
};

class HostJobSystem {
public:
    static HostJobSystem& instance();

    HostJobSystem(const HostJobSystem&) = delete;
    HostJobSystem& operator=(const HostJobSystem&) = delete;

    unsigned logical_workers() const noexcept;
    HostJobReport report() const noexcept;

    // Executes contiguous chunks covering [begin, end). The function does not
    // return until every chunk has completed. Calls made from a pool worker run
    // inline, preventing nested waits from consuming the whole pool.
    // minimum_parallel_span is the minimum useful grain per participant. A
    // range needs at least two grains before it is sent to the pool.
    void parallel_chunks(JobSite site, int begin, int end, int minimum_parallel_span,
                         const std::function<void(int, int)>& chunk);

    // Wake policy for queued chunks (D09 F1). ON (default): one notify_one per
    // queued chunk, so a group of n chunks readies at most n-1 parked workers.
    // OFF: the historical single notify_all, which readies every parked worker
    // and lets the surplus convoy on the queue mutex before parking again.
    // Host scheduling only; the barrier and the order of guest-visible commits
    // are unchanged, so the guest cannot observe which arm ran. Safe to flip
    // at any time; the fork path reads it once per group.
    void set_notify_one(bool enabled) noexcept;
    bool notify_one_enabled() const noexcept;

private:
    HostJobSystem();
    ~HostJobSystem();

    struct Impl;
    Impl* impl_ = nullptr;
};

inline unsigned host_parallel_workers() noexcept {
    return HostJobSystem::instance().logical_workers();
}

// Override the background worker count. Must be called before anything else
// touches the job system, i.e. early in main; zero restores the derived value.
// Explicit requests are bounded by the host's reported hardware concurrency.
void set_host_job_worker_target(unsigned workers);

// Same switch as HostJobSystem::set_notify_one, but it does NOT construct the
// pool, so the CLI may call it next to set_host_job_worker_target without
// caring about order (instance() fixes the worker count at first touch).
void set_host_job_notify_one(bool enabled) noexcept;

// Runs fn(i) for i in [begin, end). Small ranges stay on the calling thread.
template <typename Fn>
void host_parallel_for(JobSite site, int begin, int end, int minimum_parallel_span, Fn&& fn) {
    auto&& operation = fn;
    HostJobSystem::instance().parallel_chunks(
        site, begin, end, minimum_parallel_span, [&](int chunk_begin, int chunk_end) {
            for (int index = chunk_begin; index < chunk_end; ++index) {
                operation(index);
            }
        });
}

template <typename Fn>
void host_parallel_for(JobSite site, int begin, int end, Fn&& fn) {
    host_parallel_for(site, begin, end, 8, std::forward<Fn>(fn));
}

// Observation must not create unused workers (the lifted renderer has its own resource worker).
HostJobReport host_job_report() noexcept;

}  // namespace gekko
