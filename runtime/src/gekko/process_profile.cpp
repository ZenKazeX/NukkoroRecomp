#include "gekko/process_profile.h"

#include "gekko/profiling.h"

#if defined(SHADOW_TRACY)

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// After windows.h, which it depends on.
#include <psapi.h>
#endif

#include <chrono>
#include <cstdint>

namespace gekko {

#if defined(_WIN32)

namespace {

std::uint64_t to_u64(const FILETIME& t) noexcept {
    return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

// The cumulative counters a rate is taken from. Everything is read through
// GetCurrentProcess(): no call here can observe another process.
struct Counters {
    std::chrono::steady_clock::time_point when{};
    std::uint64_t cpu_100ns = 0;
    std::uint64_t read_bytes = 0;
    std::uint64_t write_bytes = 0;
    std::uint64_t page_faults = 0;
    // Levels, not cumulative: plotted as read.
    std::uint64_t working_set = 0;
    std::uint64_t private_bytes = 0;
    std::uint64_t handles = 0;
    bool memory_valid = false;
    bool handles_valid = false;
};

Counters read_counters() noexcept {
    Counters c;
    c.when = std::chrono::steady_clock::now();
    const HANDLE self = GetCurrentProcess();
    FILETIME creation{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(self, &creation, &exited, &kernel, &user)) {
        c.cpu_100ns = to_u64(kernel) + to_u64(user);
    }
    IO_COUNTERS io{};
    if (GetProcessIoCounters(self, &io)) {
        c.read_bytes = io.ReadTransferCount;
        c.write_bytes = io.WriteTransferCount;
    }
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(self, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                             sizeof(memory))) {
        c.page_faults = memory.PageFaultCount;
        c.working_set = memory.WorkingSetSize;
        c.private_bytes = memory.PrivateUsage;
        c.memory_valid = true;
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(self, &handles)) {
        c.handles = handles;
        c.handles_valid = true;
    }
    return c;
}

void configure_plots() noexcept {
    // Configuration is only kept by a connected viewer - on-demand profiling
    // discards anything sent before - so it is re-sent at the start of every
    // capture rather than once at startup.
    GEKKO_PROFILE_PLOT_CONFIG("host/process cpu cores", GEKKO_PLOT_NUMBER, false, true, 0x4fa8ff);
    GEKKO_PROFILE_PLOT_CONFIG("host/process working set", GEKKO_PLOT_MEMORY, true, true, 0x6fd66f);
    GEKKO_PROFILE_PLOT_CONFIG("host/process private bytes", GEKKO_PLOT_MEMORY, true, true, 0x9ad5e0);
    GEKKO_PROFILE_PLOT_CONFIG("host/process page faults/s", GEKKO_PLOT_NUMBER, false, true, 0xe0c060);
    GEKKO_PROFILE_PLOT_CONFIG("host/process io read/s", GEKKO_PLOT_MEMORY, false, true, 0xd0a0ff);
    GEKKO_PROFILE_PLOT_CONFIG("host/process io write/s", GEKKO_PLOT_MEMORY, false, true, 0xff9f6b);
    GEKKO_PROFILE_PLOT_CONFIG("host/process handles", GEKKO_PLOT_NUMBER, true, false, 0xc8c8d0);
}

}  // namespace

void publish_process_resources() noexcept {
    // One caller thread (the host service), so plain statics are safe.
    static bool anchored = false;
    static Counters last{};

    if (!GEKKO_PROFILE_CONNECTED()) {
        // A later capture must not take a rate across the time nobody was
        // watching: that single interval would be one meaningless spike. This
        // is why the caller invokes this unconditionally rather than only
        // while connected - otherwise the disconnect would never be seen.
        anchored = false;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (anchored && now - last.when < std::chrono::milliseconds(250)) {
        return;
    }
    const Counters current = read_counters();

    // Rates need a previous sample from the same capture; the configuration
    // has to reach the viewer before the first value of each plot does.
    if (!anchored) {
        configure_plots();
    }

    // Levels: meaningful on every sample, including the first of a capture.
    if (current.memory_valid) {
        GEKKO_PROFILE_PLOT("host/process working set", static_cast<double>(current.working_set));
        GEKKO_PROFILE_PLOT("host/process private bytes", static_cast<double>(current.private_bytes));
    }
    if (current.handles_valid) {
        GEKKO_PROFILE_PLOT("host/process handles", static_cast<double>(current.handles));
    }

    if (!anchored) {
        anchored = true;
        last = current;
        return;
    }
    const double seconds = std::chrono::duration<double>(current.when - last.when).count();
    if (seconds > 0.0) {
        // Every counter below is cumulative for the life of the process, so a
        // delta can only go backwards if a read failed and returned zero; skip
        // the rate for that interval rather than plot a wrapped value.
        const auto rate = [seconds](std::uint64_t now_value, std::uint64_t then_value) {
            return now_value >= then_value
                       ? static_cast<double>(now_value - then_value) / seconds
                       : -1.0;
        };
        const double cpu = rate(current.cpu_100ns, last.cpu_100ns);
        if (cpu >= 0.0) GEKKO_PROFILE_PLOT("host/process cpu cores", cpu / 1.0e7);
        const double reads = rate(current.read_bytes, last.read_bytes);
        if (reads >= 0.0) GEKKO_PROFILE_PLOT("host/process io read/s", reads);
        const double writes = rate(current.write_bytes, last.write_bytes);
        if (writes >= 0.0) GEKKO_PROFILE_PLOT("host/process io write/s", writes);
        const double faults = rate(current.page_faults, last.page_faults);
        if (faults >= 0.0) GEKKO_PROFILE_PLOT("host/process page faults/s", faults);
    }
    last = current;
}

#else

void publish_process_resources() noexcept {}

#endif

}  // namespace gekko

#endif  // SHADOW_TRACY
