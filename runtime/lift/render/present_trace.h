#pragma once

// Presentation trace (ES 2026-10-20; frame-pacing investigation, findings-20261019-ES/ES_REPORT.md section 12).
// MEASUREMENT ONLY. Enabled by the environment variable GEKKO_PRESENT_TRACE=<csv path>; off by default. When off,
// every site costs one relaxed atomic load: nothing is recorded, no extra DXGI call is made, nothing is written, and
// no behaviour changes. When on, it records - in std::chrono::steady_clock nanoseconds (QueryPerformanceCounter on
// Windows) - each packet the game hands over (push), each packet the queue drops (newest-wins, on push or on pop), each
// packet the render thread takes (pop), the frame-latency wait, the present call with DXGI's frame statistics read
// right after it (PresentCount, PresentRefreshCount, SyncRefreshCount, SyncQPCTime: which present reached the screen
// at which vblank), and the end of each packet's rendering. The rows are kept in memory and written once, when the
// renderer stops (flush).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace shadow::render::present_trace {

enum class Ev : std::uint8_t {
    Push = 1,          // a tick, b game ticks, c queue depth after the push
    DropOnPush = 2,    // a the dropped packet's tick (the queue was full)
    DropOnPop = 3,     // a the dropped packet's tick (an older packet behind the newest)
    Pop = 4,           // a tick, b game ticks
    WaitBegin = 5,     // the frame-latency waitable
    WaitEnd = 6,       // a 1 signalled, 0 timed out
    PresentBegin = 7,  // a sync interval, b flags
    PresentEnd = 8,    // a HRESULT, b GetLastPresentCount
    Stats = 9,         // a PresentCount, b PresentRefreshCount, c SyncRefreshCount, d SyncQPCTime in steady_clock ns
                       // (a = ~0: GetFrameStatistics failed, b its HRESULT)
    RenderEnd = 10,    // a tick, b 1 rendered and presented
    DropOnClose = 11,  // a the queued packet discarded when rendering stops
};

struct Row {
    std::int64_t ns;
    std::uint64_t a, b, c, d;
    std::uint8_t ev;
};

struct State {
    std::atomic<int> mode{-1};  // -1 not decided yet, 0 off, 1 on
    std::mutex mutex;           // a leaf lock: nothing is called while holding it
    std::vector<Row> rows;
    std::string path;
    bool flushed = false;
};

inline State& state() {
    static State s;
    return s;
}

#if defined(GEKKO_SHIPPING)
// GEKKO_SHIPPING: no trace in the user's exe; every record() / flush() below folds away.
constexpr bool enabled() { return false; }
#else
inline bool enabled() {
    State& s = state();
    int mode = s.mode.load(std::memory_order_relaxed);
    if (mode >= 0)
        return mode == 1;
    std::lock_guard<std::mutex> lock(s.mutex);
    mode = s.mode.load(std::memory_order_relaxed);
    if (mode < 0) {
        const char* path = std::getenv("GEKKO_PRESENT_TRACE");
        mode = path != nullptr && path[0] != '\0' ? 1 : 0;
        if (mode == 1) {
            s.path = path;
            // 2^18 rows x 48 B = 12 MiB: about 9 min of play (~480 rows / s); it grows past that. Was 2^20 = 48 MiB, committed
            // and mostly untouched - tooling memory the waste census had to subtract (ES 2026-10-20).
            s.rows.reserve(std::size_t{1} << 18);
        }
        s.mode.store(mode, std::memory_order_release);
    }
    return mode == 1;
}
#endif

inline std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline void record(Ev ev, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0, std::uint64_t d = 0) {
    if (!enabled())
        return;
    const std::int64_t t = now();
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.rows.push_back(Row{t, a, b, c, d, static_cast<std::uint8_t>(ev)});
}

// Writes the rows (once). Called by whoever stops the renderer; harmless when off or when called again.
inline void flush() {
    if (!enabled())
        return;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.flushed)
        return;
    s.flushed = true;
    std::FILE* out = std::fopen(s.path.c_str(), "wb");
    if (out == nullptr)
        return;
    std::fputs("# shadow present trace 1, time in steady_clock ns\nns,ev,a,b,c,d\n", out);
    for (const Row& r : s.rows)
        std::fprintf(out, "%lld,%u,%llu,%llu,%llu,%llu\n", static_cast<long long>(r.ns), static_cast<unsigned>(r.ev),
                     static_cast<unsigned long long>(r.a), static_cast<unsigned long long>(r.b),
                     static_cast<unsigned long long>(r.c), static_cast<unsigned long long>(r.d));
    std::fclose(out);
}

}  // namespace shadow::render::present_trace
