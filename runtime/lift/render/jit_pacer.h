#pragma once

// Just-in-time frame start (SHADOW_R2_JIT=1; off by default; docs/PERF_60FPS_2026-11-04.md, latency).
//
// Measured before (1080p, Westopolis replay): the present thread takes the swap chain's credit ~0.1 ms after a vblank,
// records and submits by ~0.5 ms, the GPU is done by ~5 ms (p99 ~8 ms) - and the frame waits for the NEXT vblank:
// ~11.5 ms of every frame is idle waiting, after the input was read. This pacer moves the start of the frame towards
// that vblank instead: after the credit it sleeps until (next vblank - lead), lead = the recent worst span from frame
// start to GPU completion plus a margin. With SHADOW_PHASE_LOCK the game's push follows the later start (the lock
// steers push -> recording begins to 2 ms), so the game reads its input later by the same amount. Nothing in a frame
// changes: the same packet, the same work, the same present at the same vblank.
//
//   * vblank grid: DXGI frame statistics (SyncRefreshCount, SyncQPCTime) after every present, fitted to a period;
//     without valid statistics (windowed under composition, occluded, at start) there is no wait.
//   * spans: the direct queue's fence of each frame, waited on by a watcher thread (one event, no polling); span =
//     GPU completion - frame start (measured 0.12 ms p50 / 0.3 ms p99 after the GPU's own end timestamp).
//   * two leads (update_lead; margin SHADOW_R2_JIT_MARGIN_US, default 2000): the COMMITTED lead (Q99 of the last 600
//     ordinary spans + margin; up at once only on a confirmed shift, down only after 600 clean frames at <= 0.25 ms/s)
//     is what the phase lock aims the game at; the RISK lead (committed, or a transient that lasts until 30 clean
//     frames) is where the frame starts - an isolated burst spends the packet's hold and never moves the game.
//     Spans that could not make any vblank (>= period - 1 ms) raise the transient but stay out of the Q99 history.
//   * the planned start is next vblank - min(risk lead, period); the phase lock's reference is next vblank -
//     committed lead (gekko/phase_lock.h, the shared controller). History: the first version (longest span of 600
//     frames, one lead for both) held a 16.8 ms burst for 10 s and moved the game's phase after it - 15-20 s of
//     worse tails after every heavy episode.
//   * VSync only. Immediate (tearing / VRR) presents never wait.

#include <d3d12.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace shadow::render {

class JitPacer {
public:
    static bool requested();  // SHADOW_R2_JIT=1

    JitPacer() = default;
    JitPacer(const JitPacer&) = delete;
    JitPacer& operator=(const JitPacer&) = delete;
    ~JitPacer();

    void start(ID3D12Fence* fence);  // the direct queue's fence; starts the watcher
    void stop();
    bool running() const noexcept { return fence_ != nullptr; }

    // After a present: DXGI's last vblank (refresh count, its steady ns).
    void note_vblank(std::uint64_t refresh_count, std::int64_t vblank_ns);
    // The vblank grid became unusable (statistics failed, occluded, resized): no wait until it is rebuilt.
    void reset_grid();
    // Before the pop: sleeps until (next vblank - risk lead) when that is later than now. start: when the frame starts
    // (steady ns); target: the phase lock's reference start, next vblank - committed lead (steady ns), 0 when nothing
    // is planned (no grid / history).
    struct Start {
        std::int64_t start = 0;
        std::int64_t target = 0;
    };
    // fill_mpx: the frame's estimated particle fill (render/particle_fill.h), megapixels; only SHADOW_R2_JIT_ASSIST uses it.
    Start wait_for_start(double fill_mpx = 0.0);

    // SHADOW_R2_JIT_ASSIST (experimental, default none): when to start a frame earlier than its planned start - as soon
    // as its packet is there, spending its hold. A frame started earlier is shown at the same vblank with the same
    // input, so starting early costs no input latency; it only finishes sooner.
    //   predict  only when the online cost model (GPU span = base + k x fill, fitted on finished frames, plus the p95
    //            of its recent absolute errors) says the frame will not fit the risk lead
    //   eager    always
    enum class Assist : std::uint8_t { None, Predict, Eager };
    static Assist assist();
    // After the frame's submit: the fence value that ends it, and the start wait_for_start returned.
    void note_submitted(std::uint64_t fence_value, std::int64_t start_ns, double fill_mpx = 0.0);

    std::int64_t lead_ns() const noexcept { return lead_ns_.load(std::memory_order_relaxed); }
    // The measured refresh period (present thread).
    std::int64_t period_ns() const noexcept { return period_ns_; }

private:
    void watch(std::stop_token stop);
    std::int64_t next_vblank_after(std::int64_t now_ns) const;
    void update_lead(std::int64_t span_ns);

    ID3D12Fence* fence_ = nullptr;
    std::jthread watcher_;
    std::mutex queue_mutex_;
    std::condition_variable_any queue_cv_;
    struct Pending {
        std::uint64_t fence = 0;
        std::int64_t start = 0;
        double fill_mpx = 0.0;
    };
    std::deque<Pending> pending_;

    // Grid (present thread only).
    bool grid_valid_ = false;
    std::uint64_t base_count_ = 0, last_count_ = 0;
    std::int64_t base_ns_ = 0, last_ns_ = 0, period_ns_ = 16'666'667;
    std::atomic<std::int64_t> period_shared_{16'666'667};  // period_ns_ for the watcher thread

    // Spans (watcher thread writes, present thread reads lead_ns_).
    std::mutex span_mutex_;
    std::array<std::int64_t, 600> spans_{};
    std::size_t span_count_ = 0, span_next_ = 0;
    std::atomic<std::int64_t> lead_ns_{0};  // the risk lead; 0 = no history: no wait
    std::atomic<std::int64_t> lead_steady_ns_{0};  // the committed lead the phase lock is measured against
    // The estimator's state (watcher thread, under span_mutex_; see update_lead).
    std::array<std::int64_t, 16> recent_{};  // every span, overload included
    std::size_t recent_count_ = 0, recent_next_ = 0;
    std::int64_t ref_q99_ = 0, ref_median_ = 0;  // the frozen reference a shift is detected against
    std::uint64_t frames_ = 0, hold_until_ = 0;
    std::uint32_t clean_ = 0, transient_clean_ = 0;
    std::int64_t transient_ = 0;
    std::vector<std::int64_t> scratch_;
    // SHADOW_R2_JIT_ASSIST=predict: the online cost model (watcher thread writes under span_mutex_, the present thread
    // reads the published coefficients).
    void update_model(double fill_mpx, std::int64_t span_ns);
    double rls_w_[2] = {0.0, 0.0};
    double rls_p_[2][2] = {{1e3, 0.0}, {0.0, 1e3}};
    bool rls_started_ = false;
    std::array<double, 120> residual_{};
    std::size_t residual_count_ = 0, residual_next_ = 0;
    std::atomic<double> model_base_ms_{0.0}, model_k_ms_{0.0}, model_allow_ms_{2.0};
    std::atomic<bool> model_ready_{false};
    std::int64_t margin_ns_ = 2'000'000;
    void* timer_ = nullptr;  // high-resolution waitable timer (present thread)
};

}  // namespace shadow::render
