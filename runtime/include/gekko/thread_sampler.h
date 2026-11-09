#pragma once

// Per-thread CPU telemetry for the presentation trace (docs/FRAME_TELEMETRY.md; MEASUREMENT ONLY, GEKKO_PRESENT_TRACE).
// Once a second a low-priority thread of its own reads, for every thread of this process, the user + kernel time and
// the number of context switches (NtQuerySystemInformation's process list: the operating system's own count of how
// often each thread was switched in - its wakeups), and the cycle count (QueryThreadCycleTime), and records them as
// present_trace ThreadCpu rows with each thread's description as its name. Nothing is estimated: a value the system
// does not give is recorded as 0 and the report says so. Inert when the trace is off: no thread is started.

#include <atomic>
#include <thread>

namespace gekko {

class ThreadSampler {
public:
    ThreadSampler() = default;
    ~ThreadSampler() { stop(); }
    ThreadSampler(const ThreadSampler&) = delete;
    ThreadSampler& operator=(const ThreadSampler&) = delete;

    // Starts the sampler when the presentation trace is on; otherwise does nothing.
    void start();
    // Takes a last sample and stops (before the trace is flushed).
    void stop();

private:
    void run();
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

}  // namespace gekko
