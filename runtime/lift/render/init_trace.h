#pragma once

// Render-thread cost trace (docs/FRAME_LATENCY_ROOT_CAUSE_ANALYSIS.md). MEASUREMENT ONLY, off by default.
// GEKKO_INIT_TRACE=<csv path> records, in std::chrono::steady_clock nanoseconds (the clock of GEKKO_PRESENT_TRACE and
// GEKKO_GPU_TRACE), every pipeline-state creation and GPU resource creation with its duration, the calling thread and
// a label; the rows are written once at process exit. Without the variable every site costs one relaxed atomic load.
// Compiled out of shipping builds (GEKKO_SHIPPING). No behaviour changes.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace shadow::render::init_trace {

#if defined(GEKKO_SHIPPING)
struct Scope {
    Scope(const char*, const wchar_t*, std::uint64_t = 0) {}
};
#else
struct Row {
    std::int64_t begin_ns, end_ns;
    std::uint32_t thread;
    std::uint64_t bytes;
    std::string kind, label;
};
struct State {
    std::atomic<int> mode{-1};
    std::mutex mutex;
    std::vector<Row> rows;
    std::string path;
    ~State() {
        if (path.empty())
            return;
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f == nullptr)
            return;
        std::fputs("begin_ns,end_ns,dur_ms,thread,kind,bytes,label\n", f);
        for (const Row& r : rows)
            std::fprintf(f, "%lld,%lld,%.4f,%u,%s,%llu,%s\n", static_cast<long long>(r.begin_ns),
                         static_cast<long long>(r.end_ns), (r.end_ns - r.begin_ns) / 1e6, r.thread, r.kind.c_str(),
                         static_cast<unsigned long long>(r.bytes), r.label.c_str());
        std::fclose(f);
    }
};
inline State& state() {
    static State s;
    return s;
}
inline bool on() {
    State& s = state();
    int m = s.mode.load(std::memory_order_relaxed);
    if (m < 0) {
        const char* p = std::getenv("GEKKO_INIT_TRACE");
        std::lock_guard<std::mutex> lock(s.mutex);
        if (p != nullptr && p[0] != '\0')
            s.path = p;
        m = s.path.empty() ? 0 : 1;
        s.mode.store(m, std::memory_order_relaxed);
    }
    return m == 1;
}
inline std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
struct Scope {
    const char* kind;
    std::string label;
    std::uint64_t bytes;
    std::int64_t begin = 0;
    bool active;
    Scope(const char* k, const wchar_t* name, std::uint64_t b = 0) : kind(k), bytes(b), active(on()) {
        if (!active)
            return;
        if (name != nullptr)
            for (const wchar_t* c = name; *c != 0; ++c)
                label.push_back(*c < 128 ? static_cast<char>(*c) : '?');
        begin = now_ns();
    }
    ~Scope() {
        if (!active)
            return;
        const std::int64_t end = now_ns();
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.rows.push_back(Row{begin, end,
#ifdef _WIN32
                             static_cast<std::uint32_t>(GetCurrentThreadId()),
#else
                             0u,
#endif
                             bytes, kind, label});
    }
};
#endif

}  // namespace shadow::render::init_trace
