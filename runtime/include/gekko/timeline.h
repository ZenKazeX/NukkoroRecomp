#pragma once

// Cross-thread timeline recorder: one timestamped record per host event on the
// game, render, presentation and audio threads, so a session can be analysed
// as one ordered history (guest audio produced -> queued -> consumed by the
// device, against pacer waits, renderer/GPU waits, presents and stalls).
//
// Off unless enable() was called: emit() is then one relaxed atomic load and a
// predictable branch. When on, each thread appends to its own preallocated
// chunks without locks or system calls; a thread takes the registry lock once,
// on its first record, and again only when a chunk fills (a counted event).
// Records past the configured capacity are counted, never blocked on.
//
// Timestamps are std::chrono::steady_clock nanoseconds (QueryPerformanceCounter
// on Windows), the same clock the rest of the runtime uses.

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>

#include "gekko/types.h"

namespace gekko::timeline {

enum class Lane : u8 {
    Game = 0,
    Render = 1,
    Present = 2,
    Audio = 3,
    Other = 4,
    Count
};

// Stable numeric ids (the analyser reads them from the file's name table).
enum class Event : u16 {
    // Game thread: pacing
    PacerWaitBegin = 1,      // a = target guest cycles, b = debt ns
    PacerWaitEnd = 2,        // a = guest cycles
    PacerBehind = 3,         // a = guest cycles, b = debt ns (boundary with no wait)
    HostServiceBegin = 4,    // a = guest cycles
    HostServiceEnd = 5,
    // Game thread: guest audio production
    GuestAudioBlock = 10,    // a = guest cycles at the block, b = frames, c = flags
    GuestAudioInterrupt = 11,// a = guest cycles, b = interrupt kind
    GuestMixBegin = 12,      // a = guest cycles
    GuestMixEnd = 13,        // a = guest cycles, b = frames mixed
    // Game thread: waits on other threads
    SegmentPushWaitBegin = 20,
    SegmentPushWaitEnd = 21, // a = waited ns
    FlipperWaitBegin = 22,
    FlipperWaitEnd = 23,
    DiscBlockingReadBegin = 24,
    DiscBlockingReadEnd = 25,
    StorageWriteBegin = 26,
    StorageWriteEnd = 27,
    SavestateBegin = 28,
    SavestateEnd = 29,
    // Render thread
    RenderSegmentBegin = 40, // a = guest frame
    RenderSegmentEnd = 41,
    FenceWaitBegin = 42,
    FenceWaitEnd = 43,
    OutputPublished = 44,    // a = generation, b = guest produce ns
    // Presentation (UI) thread
    PresentBegin = 60,       // a = generation
    PresentEnd = 61,         // a = generation, b = repeated (0/1)
    // Audio
    AudioQueued = 80,        // a = source frames accepted, b = fill after, c = dropped
    AudioWake = 81,          // a = device padding frames, b = frames requested, c = fill before
    AudioRendered = 82,      // a = source frames consumed, b = device frames written, c = silence frames
    AudioUnderrun = 83,      // a = silence frames, b = fill
    AudioDevicePosition = 84,// a = device position frames, b = QPC position ns
    AudioStateChange = 85,   // a = state
    Count
};

const char* event_name(Event event) noexcept;

// Starts recording. `capacity_records` bounds the total across all threads
// (32 bytes each). Call before the threads that record start.
void enable(const std::filesystem::path& path, std::size_t capacity_records = 1u << 24);
// Writes the file (header, name table, records per thread) and stops recording.
// Call after the recording threads have stopped.
bool finish(std::string* error = nullptr);

namespace detail {
extern std::atomic<bool> g_enabled;
void emit_slow(Lane lane, Event event, u64 a, u64 b, u32 c) noexcept;
}  // namespace detail

inline bool enabled() noexcept { return detail::g_enabled.load(std::memory_order_relaxed); }

inline void emit(Lane lane, Event event, u64 a = 0, u64 b = 0, u32 c = 0) noexcept {
    if (enabled()) detail::emit_slow(lane, event, a, b, c);
}

struct Stats {
    u64 records = 0;
    u64 dropped = 0;
    u64 threads = 0;
    u64 chunks = 0;
};
Stats stats() noexcept;

}  // namespace gekko::timeline
