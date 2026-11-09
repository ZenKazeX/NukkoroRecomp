#pragma once

// GPU-burst scheduling (ALL OPT-IN, off by default: each mode trades input latency during a burst for fewer missed
// refreshes - the user's call; docs/PERF_60FPS_2026-11-04.md "Burst scheduling").
//
// Normally a frame is recorded when the swap chain hands back its one latency credit (the vblank) and must be finished
// by the GPU before the next vblank: one refresh of GPU time. The 5K stage-start smoke burst needs 17-27 ms for a few
// frames while the driver's clock governor (it re-decides about every 500 ms from the last window's utilisation) is
// still low, and each such frame repeats the frame before. When the frames' GPU spans pass kEnterSpanNs:
//   * render-ahead (SHADOW_R2_BURST_AHEAD=1): the swap chain's maximum frame latency 1 -> 2 (DXGI adds the credit at
//     once - measured with a probe, 2026-11-05) and the game runs one frame earlier (gekko/schedule_offset.h). Every
//     frame is then recorded at a vblank and shown at the second one after it; +1 frame of input latency. Leaving:
//     latency 2 -> 1 (DXGI takes no credit back), the game one frame later again, one extra credit taken. Measured
//     limit: with all three back buffers taken the final copy waits for a flip on the GPU, and a burst whose total
//     overrun exceeds one refresh still repeats frames.
//   * burst lead (SHADOW_R2_BURST_LEAD_MS=n): eager start plus the game n ms earlier; +n ms of input latency.
// Leaving waits until the audio queue holds the lead it gives back (gekko/schedule_offset.h).
// It never changes what a frame contains: the same packets, the same game time per frame, the same pictures - only
// when they are recorded relative to the vblank, and when the game reads the input for them.
// Spans: the direct queue's fence of each frame, waited on by a watcher thread (as render/jit_pacer.h); span = GPU
// completion - start of the frame's recording.

#include <d3d12.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <stop_token>
#include <thread>

#include "render/present_trace.h"

namespace shadow::render {

class BurstAhead {
public:
    static constexpr std::int64_t kFrameNs = 16'666'667;             // one game frame of wall time (60 Hz quantum)
    static constexpr std::int64_t kEnterSpanNs = 12'000'000;         // a frame that close to the refresh: enter
    static constexpr std::int64_t kCalmSpanNs = kFrameNs / 2;        // a span this short counts as calm
    static constexpr std::uint32_t kCalmFrames = 180;                // 3 s of calm frames: exit
    // Armed only after this many frames in a row under the enter span (5 s). The boot's loading frames are long: the
    // mode entered there while the audio device was priming its queue, so the queue was primed one frame fuller, and
    // leaving took that frame out of it for good (5K and 1440p, 2026-11-05: the fill fell from ~700 to ~120 frames and
    // two underruns followed). Once armed, entering adds a frame of audio and leaving takes the same frame back.
    static constexpr std::uint32_t kArmFrames = 300;

    // Off by default. SHADOW_R2_BURST_LEAD_MS=n (1..16): burst lead - the game runs n ms earlier while a burst lasts
    // and frames start at their packet (eager); +n ms of input latency during the burst only. Else
    // SHADOW_R2_BURST_AHEAD=1: render-ahead - two frames in flight while a burst lasts; +1 frame of input latency then.
    enum class Mode : std::uint8_t { Off, Ahead, Lead };
    static Mode mode() {
        static const Mode m = [] {
            if (lead_ms_requested() > 0)
                return Mode::Lead;
            const char* v = std::getenv("SHADOW_R2_BURST_AHEAD");
            return (v != nullptr && v[0] == '1') ? Mode::Ahead : Mode::Off;
        }();
        return m;
    }
    static bool requested() { return mode() != Mode::Off; }
    // The lead the mode asks the game for while a burst lasts (gekko/schedule_offset.h).
    static std::int64_t burst_lead_ns() {
        return mode() == Mode::Lead ? static_cast<std::int64_t>(lead_ms_requested()) * 1'000'000 : kFrameNs;
    }
    // Eager start (SHADOW_R2_EAGER=1, implied by the burst lead): a frame is recorded and submitted as soon as its
    // packet is taken, and only its present waits for the swap chain's credit. The packet, its input and the vblank it
    // is shown at are the same as with the credit first; the GPU starts by the phase lock's hold (~2 ms) earlier.
    static bool eager() {
        static const bool on = [] {
            const char* v = std::getenv("SHADOW_R2_EAGER");
            return (v != nullptr && v[0] == '1') || lead_ms_requested() > 0;
        }();
        return on;
    }

private:
    static int lead_ms_requested() {
        static const int ms = [] {
            const char* v = std::getenv("SHADOW_R2_BURST_LEAD_MS");
            const int n = v != nullptr ? std::atoi(v) : 0;
            return (n >= 1 && n <= 16) ? n : 0;
        }();
        return ms;
    }

public:

    BurstAhead() = default;
    BurstAhead(const BurstAhead&) = delete;
    BurstAhead& operator=(const BurstAhead&) = delete;
    ~BurstAhead() { stop(); }

    void start(ID3D12Fence* fence) {
        if (fence_ != nullptr || fence == nullptr)
            return;
        fence_ = fence;
        watcher_ = std::jthread([this](std::stop_token stop) { watch(stop); });
    }
    void stop() {
        if (watcher_.joinable()) {
            watcher_.request_stop();
            queue_cv_.notify_all();
            watcher_.join();
        }
        fence_ = nullptr;
        std::lock_guard<std::mutex> lock(queue_mutex_);
        pending_.clear();
    }
    bool running() const noexcept { return fence_ != nullptr; }

    // After a frame's submit: the fence value that ends it and when its recording began (steady ns).
    void note_submitted(std::uint64_t fence_value, std::int64_t start_ns) {
        if (fence_ == nullptr)
            return;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            pending_.push_back(Pending{fence_value, start_ns});
        }
        queue_cv_.notify_one();
    }

    // What the spans ask for (render thread reads; the renderer applies it).
    bool want_ahead() const noexcept { return want_.load(std::memory_order_acquire); }
    // The span that made the last decision (trace).
    std::int64_t deciding_span_ns() const noexcept { return deciding_span_.load(std::memory_order_relaxed); }
    // The renderer had to leave the mode without the spans (a new or resized swap chain): start calm again.
    void force_calm() noexcept {
        calm_.store(0, std::memory_order_relaxed);
        want_.store(false, std::memory_order_release);
    }

private:
    struct Pending {
        std::uint64_t fence = 0;
        std::int64_t start = 0;
    };

    static std::int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void on_span(std::int64_t span) {
        if (!armed_) {
            arm_calm_ = span <= kEnterSpanNs ? arm_calm_ + 1u : 0u;
            armed_ = arm_calm_ >= kArmFrames;
            return;
        }
        if (!want_.load(std::memory_order_relaxed)) {
            if (span > kEnterSpanNs) {
                calm_.store(0, std::memory_order_relaxed);
                deciding_span_.store(span, std::memory_order_relaxed);
                want_.store(true, std::memory_order_release);
            }
            return;
        }
        const std::uint32_t calm = span < kCalmSpanNs ? calm_.load(std::memory_order_relaxed) + 1u : 0u;
        calm_.store(calm, std::memory_order_relaxed);
        if (calm >= kCalmFrames) {
            deciding_span_.store(span, std::memory_order_relaxed);
            want_.store(false, std::memory_order_release);
        }
    }

    void watch(std::stop_token stop) {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        while (!stop.stop_requested()) {
            Pending item;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                if (!queue_cv_.wait(lock, stop, [this] { return !pending_.empty(); }))
                    break;
                item = pending_.front();
                pending_.pop_front();
            }
            const bool done = fence_->GetCompletedValue() >= item.fence;
            if (!done && (event == nullptr || FAILED(fence_->SetEventOnCompletion(item.fence, event))))
                continue;
            if (!done) {
                while (WaitForSingleObject(event, 100) == WAIT_TIMEOUT) {
                    if (stop.stop_requested()) {
                        CloseHandle(event);
                        return;
                    }
                }
            }
            // A completion seen late (this thread was behind) only makes the span longer: errs towards entering.
            on_span(now_ns() - item.start);
        }
        if (event != nullptr)
            CloseHandle(event);
    }

    ID3D12Fence* fence_ = nullptr;
    std::jthread watcher_;
    std::mutex queue_mutex_;
    std::condition_variable_any queue_cv_;
    std::deque<Pending> pending_;
    std::atomic<bool> want_{false};
    std::atomic<std::uint32_t> calm_{0};
    std::atomic<std::int64_t> deciding_span_{0};
    bool armed_ = false;          // watcher thread only
    std::uint32_t arm_calm_ = 0;  // watcher thread only
};

}  // namespace shadow::render
