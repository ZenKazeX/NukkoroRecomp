#include "render/jit_pacer.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "render/present_trace.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace shadow::render {
namespace {

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr std::size_t kMinSpans = 60;              // history before the first wait
constexpr std::int64_t kMinSleep = 50'000;          // a planned start less than 50 us away: no sleep
constexpr std::int64_t kSpinBefore = 200'000;       // the timer wakes 0.2 ms early; the rest yields
constexpr std::uint64_t kMinGridRefreshes = 60;     // refreshes behind the period estimate before it is used
constexpr std::uint64_t kRebaseRefreshes = 6000;    // ~100 s: the period estimate follows slow drift
constexpr std::uint64_t kCommitFrames = 120;        // a confirmed shift's floor holds this long
constexpr std::uint32_t kCleanBeforeFall = 600;     // clean frames before the committed lead may fall
constexpr std::int64_t kFallPerCleanFrame = 4'167;  // then at most 0.25 ms per second (60 Hz)
constexpr std::uint32_t kTransientClean = 30;       // a transient expires after this many clean frames

}  // namespace

bool JitPacer::requested() {
    static const bool on = [] {
        const char* v = std::getenv("SHADOW_R2_JIT");
        return v != nullptr && v[0] == '1';
    }();
    return on;
}

JitPacer::~JitPacer() { stop(); }

void JitPacer::start(ID3D12Fence* fence) {
    if (fence_ != nullptr || fence == nullptr)
        return;
    fence_ = fence;
    if (const char* v = std::getenv("SHADOW_R2_JIT_MARGIN_US"))
        margin_ns_ = std::clamp<std::int64_t>(std::strtoll(v, nullptr, 10), 0, 10'000) * 1000;
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    watcher_ = std::jthread([this](std::stop_token stop) { watch(stop); });
}

void JitPacer::stop() {
    if (fence_ == nullptr)
        return;
    watcher_.request_stop();
    queue_cv_.notify_all();
    watcher_ = std::jthread();  // joins
    if (timer_ != nullptr)
        CloseHandle(static_cast<HANDLE>(timer_));
    timer_ = nullptr;
    fence_ = nullptr;
    pending_.clear();
    reset_grid();
}

void JitPacer::note_vblank(std::uint64_t refresh_count, std::int64_t vblank_ns) {
    if (refresh_count == 0 || vblank_ns <= 0)
        return;
    if (!grid_valid_ || refresh_count < last_count_ || refresh_count - base_count_ > kRebaseRefreshes) {
        // A fresh base: after a reset, a counter that went back, or every ~100 s (a new estimate over the next span).
        if (grid_valid_ && refresh_count > base_count_ + kMinGridRefreshes && refresh_count >= last_count_) {
            // Keep the measured period across the re-base.
            period_ns_ = (vblank_ns - base_ns_) / static_cast<std::int64_t>(refresh_count - base_count_);
        }
        base_count_ = refresh_count;
        base_ns_ = vblank_ns;
        grid_valid_ = true;
    }
    last_count_ = refresh_count;
    last_ns_ = vblank_ns;
    if (refresh_count - base_count_ >= kMinGridRefreshes)
        period_ns_ = (vblank_ns - base_ns_) / static_cast<std::int64_t>(refresh_count - base_count_);
    // Only a plausible refresh (10 Hz .. 1 kHz) makes a grid: a zero period divides by zero in next_vblank_after, and a
    // tiny one turns the hold's wrap loops into millions of steps. Bogus frame statistics start a fresh grid instead.
    if (period_ns_ < 1'000'000 || period_ns_ > 100'000'000) {
        reset_grid();
        period_ns_ = 16'666'667;  // the initial assumption until a grid measures it again
    }
    period_shared_.store(period_ns_, std::memory_order_relaxed);
}

void JitPacer::reset_grid() {
    grid_valid_ = false;
    base_count_ = last_count_ = 0;
    base_ns_ = last_ns_ = 0;
}

std::int64_t JitPacer::next_vblank_after(std::int64_t t) const {
    if (t < last_ns_ || period_ns_ <= 0)
        return last_ns_;
    return last_ns_ + ((t - last_ns_) / period_ns_ + 1) * period_ns_;
}

JitPacer::Assist JitPacer::assist() {
    static const Assist mode = [] {
        const char* v = std::getenv("SHADOW_R2_JIT_ASSIST");
        if (v == nullptr) return Assist::None;
        const std::string s(v);
        return s == "predict" ? Assist::Predict : s == "eager" ? Assist::Eager : Assist::None;
    }();
    return mode;
}

JitPacer::Start JitPacer::wait_for_start(double fill_mpx) {
    const std::int64_t now = now_ns();
    const std::int64_t lead = lead_ns_.load(std::memory_order_relaxed);
    // A grid needs a period measured over kMinGridRefreshes: before that (and with no span history) nothing waits.
    const bool grid_ready = grid_valid_ && last_count_ >= base_count_ + kMinGridRefreshes;
    if (fence_ == nullptr || !grid_ready || lead <= 0) {
        present_trace::record(present_trace::Ev::JitWait, 0u, static_cast<std::uint64_t>(std::max<std::int64_t>(lead, 0)), 0u);
        return {now, 0};
    }
    // Always a planned start once there is a grid and a lead, so the phase lock measures against one thing: a lead of
    // a whole period plans the start at the vblank just passed - the credit, i.e. the frame starts as it did without
    // the pacer. (The first A/B switched between planned and unplanned whenever a heavy frame raised the lead, and
    // the lock re-converged after every switch.)
    const std::int64_t vblank = next_vblank_after(now);
    // The render target follows the risk lead (it may spend the packet's hold); the phase lock is measured against the
    // committed lead, so a transient does not move the game's phase (docs/PERF_60FPS_2026-11-04.md, shared phase).
    const std::int64_t committed = std::min(lead_steady_ns_.load(std::memory_order_relaxed), period_ns_);
    std::int64_t target = vblank - std::max(std::min(lead, period_ns_), committed);
    const std::int64_t reference = vblank - committed;
    // SHADOW_R2_JIT_ASSIST: start now (the packet is here) instead of at the target - same vblank, same input.
    const Assist mode = assist();
    if (mode == Assist::Eager) {
        target = now;
    } else if (mode == Assist::Predict && model_ready_.load(std::memory_order_relaxed)) {
        const double predicted = model_base_ms_.load(std::memory_order_relaxed) +
                                 model_k_ms_.load(std::memory_order_relaxed) * fill_mpx;
        const double allow = model_allow_ms_.load(std::memory_order_relaxed);
        const double margin_ms = static_cast<double>(margin_ns_) / 1e6;
        const bool alarm = (predicted + allow + margin_ms) * 1e6 > static_cast<double>(std::min(lead, period_ns_));
        if (alarm) target = now;
        present_trace::record(present_trace::Ev::JitPredict, static_cast<std::uint64_t>(predicted * 1e6),
                              static_cast<std::uint64_t>(allow * 1e6), alarm ? 1u : 0u,
                              static_cast<std::uint64_t>(fill_mpx * 1e6));
    }
    std::int64_t t = now;
    if (target - now > kMinSleep) {
        const std::int64_t sleep = target - now - kSpinBefore;
        if (sleep > 0 && timer_ != nullptr) {
            LARGE_INTEGER due{};
            due.QuadPart = -(sleep / 100);  // relative, 100 ns units
            if (SetWaitableTimer(static_cast<HANDLE>(timer_), &due, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(static_cast<HANDLE>(timer_), static_cast<DWORD>(sleep / 1'000'000 + 2));
        } else if (sleep >= 2'000'000) {
            // No high-resolution timer: a coarse sleep for the whole milliseconds, not a yield spin for the period.
            Sleep(static_cast<DWORD>(sleep / 1'000'000 - 1));
        }
        while ((t = now_ns()) < target)
            std::this_thread::yield();
    }
    // Uncovered demand: how much of the requested lead was not available (the frame started later than asked, or the
    // request exceeds the time to the vblank). Visible, never clamped away.
    const std::int64_t uncovered = std::max<std::int64_t>(0, lead - (vblank - t));
    present_trace::record(present_trace::Ev::JitWait, static_cast<std::uint64_t>(t - now),
                          static_cast<std::uint64_t>(lead), static_cast<std::uint64_t>(vblank),
                          static_cast<std::uint64_t>(uncovered));
    return {t, reference};
}

void JitPacer::note_submitted(std::uint64_t fence_value, std::int64_t start_ns, double fill_mpx) {
    if (fence_ == nullptr)
        return;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        pending_.push_back(Pending{fence_value, start_ns, fill_mpx});
    }
    queue_cv_.notify_one();
}

void JitPacer::watch(std::stop_token stop) {
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
            continue;  // no way to wait for it: a span measured now would be too short and lower the lead - drop it
        if (!done) {
            // Bounded waits so a stop is seen even if the GPU never signals (a removed device).
            while (WaitForSingleObject(event, 100) == WAIT_TIMEOUT) {
                if (stop.stop_requested()) {
                    CloseHandle(event);
                    return;
                }
            }
        }
        // A completion seen late (this thread was behind) only makes the span longer: the lead errs towards no wait.
        const std::int64_t span = now_ns() - item.start;
        present_trace::record(present_trace::Ev::FrameSpan, static_cast<std::uint64_t>(item.start),
                              static_cast<std::uint64_t>(std::max<std::int64_t>(span, 0)), item.fence);
        update_lead(span);
        if (assist() == Assist::Predict)
            update_model(item.fill_mpx, span);
    }
    if (event != nullptr)
        CloseHandle(event);
}

void JitPacer::update_model(double fill_mpx, std::int64_t span_ns) {
    // Recursive least squares for span_ms = base + k * fill_mpx (forgetting factor 0.995: ~200 frames of memory),
    // fitted only on finished frames; the allowance is the p95 of the last 120 absolute errors. Offline replay on four
    // resolutions (tools/perf/fill_model.py): bursts above the recent Q99 caught 58-96 %, first frames 48-88 %.
    constexpr double kLambda = 0.995;
    std::lock_guard<std::mutex> lock(span_mutex_);
    const double y = static_cast<double>(span_ns) / 1e6;
    if (!rls_started_) {
        rls_w_[0] = y;
        rls_started_ = true;
    }
    const double predicted = rls_w_[0] + rls_w_[1] * fill_mpx;
    const double error = y - predicted;
    const double phi[2] = {1.0, fill_mpx};
    const double pphi[2] = {rls_p_[0][0] * phi[0] + rls_p_[0][1] * phi[1], rls_p_[1][0] * phi[0] + rls_p_[1][1] * phi[1]};
    const double den = kLambda + phi[0] * pphi[0] + phi[1] * pphi[1];
    const double gain[2] = {pphi[0] / den, pphi[1] / den};
    rls_w_[0] += gain[0] * error;
    rls_w_[1] += gain[1] * error;
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 2; ++c) rls_p_[r][c] = (rls_p_[r][c] - gain[r] * pphi[c]) / kLambda;
    residual_[residual_next_] = std::abs(error);
    residual_next_ = (residual_next_ + 1) % residual_.size();
    residual_count_ = std::min(residual_count_ + 1, residual_.size());
    if (residual_count_ >= 20u) {
        std::array<double, 120> sorted = residual_;
        const std::size_t n = residual_count_, k = std::min(n - 1u, static_cast<std::size_t>(0.95 * static_cast<double>(n)));
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(k),
                         sorted.begin() + static_cast<std::ptrdiff_t>(n));
        model_allow_ms_.store(sorted[k], std::memory_order_relaxed);
        model_base_ms_.store(rls_w_[0], std::memory_order_relaxed);
        model_k_ms_.store(rls_w_[1], std::memory_order_relaxed);
        model_ready_.store(true, std::memory_order_relaxed);
    }
}

namespace {

// The value at quantile p of the first n entries (n > 0); `scratch` is reused.
std::int64_t quantile(const std::int64_t* values, std::size_t n, double p, std::vector<std::int64_t>& scratch) {
    scratch.assign(values, values + n);
    const std::size_t k = std::min(n - 1u, static_cast<std::size_t>(std::ceil(static_cast<double>(n) * p)) - 1u);
    std::nth_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(k), scratch.end());
    return scratch[k];
}

}  // namespace

void JitPacer::update_lead(std::int64_t span) {
    // Two leads (2026-11-04, after Astra's recommendation; docs/PERF_60FPS_2026-11-04.md):
    //   committed  what the phase lock aims the game at: Q99 of the last 600 ordinary spans + margin. It rises at
    //              once only on a CONFIRMED shift (3 of the last 8 spans above the frozen reference Q99 by 0.75 ms, or
    //              the median of the last 16 above the frozen reference median by max(1 ms, 25 %)) - to at least
    //              Q90 of the last 16 + margin, held kCommitFrames - and falls only after 600 clean frames, at most
    //              0.25 ms per second.
    //   risk       the render start's lead: committed, or more while a transient lasts (a span + margin above it),
    //              until 30 clean frames have passed. An isolated burst spends the packet's hold; it never moves
    //              the game's phase.
    // A span that could not make any vblank (>= period - 1 ms) is overload: it raises the transient (the frame then
    // starts at the credit) but stays out of the Q99 history, where it would only switch the pacer off for minutes.
    std::lock_guard<std::mutex> lock(span_mutex_);
    span = std::max<std::int64_t>(span, 0);
    const std::int64_t period = period_shared_.load(std::memory_order_relaxed);
    const bool hopeless = span >= period - 1'000'000;
    recent_[recent_next_] = span;
    recent_next_ = (recent_next_ + 1) % recent_.size();
    recent_count_ = std::min(recent_count_ + 1, recent_.size());
    if (!hopeless) {
        spans_[span_next_] = span;
        span_next_ = (span_next_ + 1) % spans_.size();
        span_count_ = std::min(span_count_ + 1, spans_.size());
    }
    if (span_count_ < kMinSpans)
        return;
    ++frames_;

    const std::int64_t q99 = quantile(spans_.data(), span_count_, 0.99, scratch_);
    std::int64_t committed = lead_steady_ns_.load(std::memory_order_relaxed);
    if (committed <= 0) {  // first estimate
        committed = q99 + margin_ns_;
        ref_q99_ = q99;
        ref_median_ = quantile(spans_.data(), span_count_, 0.5, scratch_);
    }
    // Confirmed shift: the last 8 / 16 spans against the frozen reference.
    std::size_t above = 0;
    std::array<std::int64_t, 16> last16{};
    const std::size_t n16 = std::min<std::size_t>(16u, recent_count_);
    for (std::size_t i = 1; i <= n16; ++i) {
        last16[i - 1] = recent_[(recent_next_ + recent_.size() - i) % recent_.size()];
        if (i <= 8u && last16[i - 1] > ref_q99_ + 750'000) ++above;
    }
    bool shifted = false;
    if (n16 == 16u) {
        const std::int64_t median16 = quantile(last16.data(), 16u, 0.5, scratch_);
        shifted = above >= 3u || median16 > ref_median_ + std::max<std::int64_t>(1'000'000, ref_median_ / 4);
    }
    if (shifted) {
        const std::int64_t floor = quantile(last16.data(), n16, 0.9, scratch_) + margin_ns_;
        committed = std::max(committed, std::min(floor, period));
        hold_until_ = frames_ + kCommitFrames;
        clean_ = 0;
        ref_q99_ = q99;
        ref_median_ = quantile(spans_.data(), span_count_, 0.5, scratch_);
    } else {
        const std::int64_t candidate = q99 + margin_ns_;
        if (candidate > committed && frames_ >= hold_until_) {
            committed = candidate;  // the ordinary tail grew without a sudden shift
            ref_q99_ = q99;
        } else if (span + margin_ns_ <= committed) {
            if (++clean_ >= kCleanBeforeFall && frames_ >= hold_until_ && candidate < committed) {
                committed = std::max(candidate, committed - kFallPerCleanFrame);
                ref_q99_ = q99;
                ref_median_ = quantile(spans_.data(), span_count_, 0.5, scratch_);
            }
        } else {
            clean_ = 0;
        }
    }
    lead_steady_ns_.store(committed, std::memory_order_relaxed);

    // Risk: committed, raised by a transient until kTransientClean clean frames.
    if (span + margin_ns_ > committed) {
        transient_ = std::max(transient_, span + margin_ns_);
        transient_clean_ = 0;
    } else if (transient_ > 0 && ++transient_clean_ >= kTransientClean) {
        transient_ = 0;
    }
    lead_ns_.store(std::max(committed, transient_), std::memory_order_relaxed);
}

}  // namespace shadow::render
