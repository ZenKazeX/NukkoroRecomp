
#include "gekko/host_jobs.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    using namespace gekko;
    using Clock = std::chrono::steady_clock;
    const bool observe = argc > 1 && std::string(argv[1]) == "--observe";
    // A report must neither instantiate the pool nor fix its worker policy.
    const auto unused = host_job_report();
    if (unused.logical_workers != 1u || unused.queue_capacity != 0u || unused.parallel_groups != 0u) return 1;
    set_host_job_worker_target(1u);
    const auto configured = host_job_report();
    if (configured.requested_workers != 1u || configured.logical_workers != 1u || configured.queue_capacity != 0u) return 1;
    if (argc > 1 && std::string(argv[1]) == "--oversized") {
        set_host_job_worker_target(std::numeric_limits<unsigned>::max());
        const auto report = HostJobSystem::instance().report();
        const unsigned hardware = std::max(std::thread::hardware_concurrency(), 1u);
        const bool bounded = report.logical_workers >= 1u &&
            report.logical_workers - 1u <= hardware && report.queue_capacity <= hardware;
        std::cout << "oversized request: hardware=" << hardware
                  << " workers=" << report.logical_workers - 1u
                  << " queue allocation=" << report.queue_capacity << "\n";
        return bounded ? 0 : 1;
    }
    set_host_job_worker_target(2u);
    auto& jobs = HostJobSystem::instance();
    if (jobs.logical_workers() != 3u) return 77;
    const auto active = host_job_report();
    if (active.logical_workers != jobs.logical_workers() || active.queue_capacity != jobs.report().queue_capacity) return 1;
    constexpr unsigned callers = 24;
    std::mutex mutex;
    std::condition_variable ready;
    bool release = false;
    std::atomic<unsigned> blocked_workers{0}, entered{0}, exceptions{0};
    std::array<std::atomic<unsigned>, callers * 3> visits{};
    const auto park = [&] {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [&] { return release; });
    };
    // Occupy both real workers before submitting independent competing callers.
    std::thread blocker([&] {
        jobs.parallel_chunks(JobSite::Unknown, 0, 3, 1, [&](int begin, int) {
            if (begin != 0) { ++blocked_workers; ready.notify_all(); park(); }
        });
    });
    bool setup_ok = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        setup_ok = ready.wait_for(lock, std::chrono::seconds(5),
                                 [&] { return blocked_workers == 2u; });
    }
    std::vector<std::thread> threads;
    for (unsigned caller = 0; caller < callers; ++caller) {
        threads.emplace_back([&, caller] {
            try {
                jobs.parallel_chunks(JobSite::Unknown, 0, 3, 1, [&](int begin, int end) {
                    if (begin == 0) { ++entered; ready.notify_all(); park(); }
                    for (int i = begin; i < end; ++i) ++visits[caller * 3u + i];
                    if (caller == callers - 1u && begin == 1)
                        throw std::runtime_error("expected chunk failure");
                });
            } catch (const std::runtime_error&) { ++exceptions; }
        });
    }
    HostJobReport saturated;
    {
        std::unique_lock<std::mutex> lock(mutex);
        setup_ok = ready.wait_for(lock, std::chrono::seconds(5),
                                 [&] { return entered == callers; }) && setup_ok;
        saturated = jobs.report();
        release = true;
    }
    ready.notify_all();
    blocker.join();
    for (auto& thread : threads) thread.join();
    bool exact = setup_ok && exceptions == 1u;
    for (const auto& visit : visits) exact = exact && visit == 1u;
    // Calls opened on a worker must run inline and may not deadlock the pool.
    std::atomic<unsigned> nested{0};
    jobs.parallel_chunks(JobSite::Unknown, 0, 3, 1, [&](int, int) {
        host_parallel_for(JobSite::Unknown, 0, 17, 1, [&](int) { ++nested; });
    });
    exact = exact && nested == 51u;
    const auto start = Clock::now();
    std::atomic<unsigned> work{0};
    for (unsigned iteration = 0; iteration < 5000; ++iteration)
        host_parallel_for(JobSite::Unknown, 0, 24, 1, [&](int) { ++work; });
    exact = exact && work == 120000u;
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::cout << "{\"exact_partitions_and_exception_join\":" << (exact ? "true" : "false")
              << ",\"background_workers\":2,\"callers\":24,\"saturated_queue_peak\":"
              << saturated.queue_peak << ",\"queue_capacity\":" << saturated.queue_capacity
              << ",\"steady_groups\":5000,\"steady_seconds\":" << seconds << "}\n";
    const bool bounded = saturated.queue_peak <= 2u && saturated.queue_capacity <= 2u;
    return exact && (observe || bounded) ? 0 : 1;
}
