#include "gekko/audio_runtime.h"
#include "gekko/thread_name.h"  // ES-T1
#include "gekko/audio_audit.h"
#include "gekko/schedule_offset.h"
#include "native_memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <numeric>
#include <system_error>
#include <thread>

// SSE2 is the x86-64 baseline. Spelling the kernel in it gives every /arch
// build the same summation order, so resampled output is one numeric model.
// Other targets (Android arm64) spell the same four lanes in plain C++, in the
// same order, so they compute the same model.
#if defined(_M_X64) || defined(__x86_64__)
#define SHADOW_AUDIO_SSE2 1
#include <emmintrin.h>
#else
#define SHADOW_AUDIO_SSE2 0
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propidl.h>
// Linked here so integration needs no CMake change for the endpoint code.
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")
#endif

// After the Windows headers above on purpose: in a Tracy build this pulls in
// TracyD3D12, which pulls in windows.h, so this file's own WIN32_LEAN_AND_MEAN
// and NOMINMAX guards are the ones that take effect either way.
#include "gekko/profiling.h"
#include "render/present_trace.h"

namespace gekko {
namespace {

constexpr AudioFrameIndex64 kScratchFrames = 512;  // device frames resampled per inner step
constexpr AudioFrameIndex64 kFeedFrames = 1024;    // source frames moved ring -> resampler per step
constexpr AudioFrameIndex64 kMinRingFrames = 64;
constexpr AudioFrameIndex64 kMaxRingFrames = u64(1) << 24;

// Realized response measured by audio_runtime_tests (analytic, 10 Hz grid):
// flat within 0.04 dB to 14 kHz, >= 86 dB down from the band limit to 96 kHz.
// The cutoff sits below the band limit so the transition ends at it. Chosen by
// a beta x cutoff sweep over 80 taps (scratch/.../audio-runtime/tune_kernel.cpp).
constexpr double kCutoffFraction = 0.93;
constexpr double kKaiserBeta = 8.5;
constexpr double kPi = 3.14159265358979323846;

u64 round_up_pow2(u64 value) noexcept {
    u64 result = 1;
    while (result < value) result <<= 1u;
    return result;
}

// Modified Bessel function of the first kind, order 0 (Kaiser window).
double bessel_i0(double x) noexcept {
    double sum = 1.0;
    double term = 1.0;
    const double quarter = x * x / 4.0;
    for (int k = 1; k < 500; ++k) {
        term *= quarter / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

float horizontal_sum(const float (&value)[4]) noexcept {
    // A fixed order: every call and every build adds the lanes identically.
    return (value[0] + value[2]) + (value[1] + value[3]);
}

#if SHADOW_AUDIO_SSE2
float horizontal_sum(__m128 lanes) noexcept {
    alignas(16) float value[4];
    _mm_store_ps(value, lanes);
    return horizontal_sum(value);
}
#endif

// Big-endian s16 pairs to native. The AI DMA stream is interleaved L/R.
inline void convert_be_frames(s16* dst, const u8* src, u64 frames) noexcept {
    const u64 samples = frames * 2u;
    u64 i = 0;
#if SHADOW_AUDIO_SSE2
    const u64 simd = samples & ~static_cast<u64>(7u);
    for (; i < simd; i += 8u) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i * 2u));
        v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), v);
    }
#endif
    for (; i < samples; ++i) {
        dst[i] = static_cast<s16>(load_be16(src + i * 2u));
    }
}

// Source frames covering `units` units of 1/`per_second` seconds, rounded up.
// 64-bit host arithmetic: a 32-bit (units * num) wrap would mis-size the prime
// and look like starvation or extra latency.
AudioFrameIndex64 source_frames_ceil(u64 units, u64 per_second) noexcept {
    if (units == 0u || per_second == 0u) return 0u;
    if (kAudioSourceRateDen > std::numeric_limits<u64>::max() / per_second) {
        return std::numeric_limits<u64>::max();
    }
    const u64 denominator = kAudioSourceRateDen * per_second;
    if (units > std::numeric_limits<u64>::max() / kAudioSourceRateNum) {
        return std::numeric_limits<u64>::max();
    }
    const u64 numerator = units * kAudioSourceRateNum;
    return (numerator + denominator - 1u) / denominator;
}

const char* kind_name(const AudioDeviceFormat& format) noexcept {
    switch (format.kind) {
        case AudioSampleKind::Float32: return "float32";
        case AudioSampleKind::Float64: return "float64";
        case AudioSampleKind::Int16: return "pcm16";
        case AudioSampleKind::Int24: return "pcm24";
        case AudioSampleKind::Int32: return format.valid_bits == 24 ? "pcm32/24" : "pcm32";
    }
    return "unknown";
}

}  // namespace

u32 AudioDeviceFormat::bytes_per_sample() const noexcept {
    switch (kind) {
        case AudioSampleKind::Float32: return 4u;
        case AudioSampleKind::Float64: return 8u;
        case AudioSampleKind::Int16: return 2u;
        case AudioSampleKind::Int24: return 3u;
        case AudioSampleKind::Int32: return 4u;
    }
    return 0u;
}

// ---------------------------------------------------------------------------
// AudioResampler
// ---------------------------------------------------------------------------

bool AudioResampler::configure(u64 source_num, u64 source_den, u32 output_rate, bool fine_ratio) {
    up_ = 0;
    down_ = 0;
    trim_ = 0;
    if (source_num == 0u || source_den == 0u || output_rate == 0u) return false;
    if (source_den > std::numeric_limits<u64>::max() / output_rate) return false;
    u64 up = static_cast<u64>(output_rate) * source_den;
    u64 down = source_num;
    const u64 divisor = std::gcd(up, down);
    up /= divisor;
    down /= divisor;
    if (fine_ratio) {
        up *= kFineScale;
        down *= kFineScale;
    }
    // phase_ * kPhases and phase_ + down_ must stay far inside 64 bits.
    if (up > (u64(1) << 40) || down > (u64(1) << 40)) return false;

    const double source_hz = static_cast<double>(source_num) / static_cast<double>(source_den);
    const double band = std::min(source_hz, static_cast<double>(output_rate)) / 2.0;
    u32 half = kBaseHalfTaps;
    if (static_cast<double>(output_rate) < source_hz) {
        // A lower band limit needs a proportionally longer kernel for the same
        // transition shape, so the measured design holds when downsampling.
        half = static_cast<u32>(std::ceil(kBaseHalfTaps * source_hz / output_rate));
        half += half & 1u;  // even: taps stay a multiple of 4 for the SSE loop
        if (half > 1024u) return false;
    }
    const u32 taps = half * 2u;
    const double cutoff = kCutoffFraction * band / source_hz;  // cycles per input frame
    const double window_norm = bessel_i0(kKaiserBeta);

    // Row r is the kernel at phase r / kPhases: tap m weights the input frame at
    // offset x = m - (half - 1) - phase from the output instant. Row kPhases
    // (phase 1.0) exists only to give the last row its slope. The kernel is zero
    // at |x| >= half, so row 0's x = +half tap and row kPhases' x = -half tap
    // agree and adjacent centers join without a seam.
    std::vector<double> rows((kPhases + 1u) * taps, 0.0);
    for (u32 row = 0; row <= kPhases; ++row) {
        const double phase = static_cast<double>(row) / kPhases;
        double sum = 0.0;
        for (u32 tap = 0; tap < taps; ++tap) {
            const double x = static_cast<double>(tap) - static_cast<double>(half - 1u) - phase;
            if (std::fabs(x) >= static_cast<double>(half)) continue;
            const double arg = 2.0 * cutoff * x;
            const double sinc = std::fabs(arg) < 1e-12 ? 1.0 : std::sin(kPi * arg) / (kPi * arg);
            const double q = x / half;
            const double value =
                2.0 * cutoff * sinc * bessel_i0(kKaiserBeta * std::sqrt(1.0 - q * q)) / window_norm;
            rows[row * taps + tap] = value;
            sum += value;
        }
        // Unity DC gain at every phase: no phase-dependent gain ripple.
        for (u32 tap = 0; tap < taps; ++tap) rows[row * taps + tap] /= sum;
    }
    base_.assign(static_cast<std::size_t>(kPhases) * taps, 0.0f);
    slope_.assign(static_cast<std::size_t>(kPhases) * taps, 0.0f);
    for (u32 row = 0; row < kPhases; ++row) {
        for (u32 tap = 0; tap < taps; ++tap) {
            const float here = static_cast<float>(rows[row * taps + tap]);
            const float next = static_cast<float>(rows[(row + 1u) * taps + tap]);
            base_[row * taps + tap] = here;
            slope_[row * taps + tap] = next - here;
        }
    }
    capacity_ = static_cast<AudioFrameIndex64>(taps) + kHistoryFrames;
    left_.assign(static_cast<std::size_t>(capacity_), 0.0f);
    right_.assign(static_cast<std::size_t>(capacity_), 0.0f);
    up_ = up;
    down_ = down;
    half_taps_ = half;
    taps_ = taps;
    source_hz_ = source_hz;
    cutoff_hz_ = kCutoffFraction * band;
    reset();
    return true;
}

void AudioResampler::reset() noexcept {
    std::fill(left_.begin(), left_.end(), 0.0f);
    std::fill(right_.begin(), right_.end(), 0.0f);
    // half - 1 frames of zero history: the first output instant sits exactly on
    // the first frame pushed after the reset.
    write_ = half_taps_ != 0u ? half_taps_ - 1u : 0u;
    center_ = write_;
    phase_ = 0;
}

AudioFrameIndex64 AudioResampler::input_needed(AudioFrameIndex64 output_frames) const noexcept {
    if (!configured() || output_frames == 0u) return 0u;
    const AudioFrameIndex64 n_minus_1 = output_frames - 1u;
    // (n-1)*down_ in 32-bit (or an overflowing u64) wraps the output instant
    // backward, so the consumer thinks it needs no input and emits silence
    // while the ring still holds samples.
    const u64 step = this->step();
    if (step != 0u && n_minus_1 > (std::numeric_limits<u64>::max() - phase_) / step) {
        return input_room();
    }
    const AudioFrameIndex64 advance = (phase_ + n_minus_1 * step) / up_;
    if (center_ > std::numeric_limits<u64>::max() - advance) {
        return input_room();
    }
    const AudioFrameIndex64 last_center = center_ + advance;
    if (last_center > std::numeric_limits<u64>::max() - (half_taps_ + 1u)) {
        return input_room();
    }
    const AudioFrameIndex64 required = last_center + half_taps_ + 1u;
    return required > write_ ? required - write_ : 0u;
}

AudioFrameIndex64 AudioResampler::input_room() const noexcept {
    if (!configured()) return 0u;
    const AudioFrameIndex64 live = center_ + 1u - half_taps_;
    return capacity_ - (write_ - live);
}

AudioFrameIndex64 AudioResampler::push(const s16* interleaved, AudioFrameIndex64 frames) noexcept {
    if (!configured() || frames == 0u || interleaved == nullptr) return 0u;
    const AudioFrameIndex64 live = center_ + 1u - half_taps_;  // oldest frame still needed
    if (live != 0u && frames > capacity_ - write_) {
        const AudioFrameIndex64 held = write_ - live;
        std::memmove(left_.data(), left_.data() + static_cast<std::size_t>(live),
                     static_cast<std::size_t>(held) * sizeof(float));
        std::memmove(right_.data(), right_.data() + static_cast<std::size_t>(live),
                     static_cast<std::size_t>(held) * sizeof(float));
        write_ = held;
        center_ -= live;
    }
    const AudioFrameIndex64 accepted = std::min(frames, capacity_ - write_);
    float* const left = left_.data() + static_cast<std::size_t>(write_);
    float* const right = right_.data() + static_cast<std::size_t>(write_);
    for (AudioFrameIndex64 i = 0; i < accepted; ++i) {
        const std::size_t at = static_cast<std::size_t>(i);
        left[at] = static_cast<float>(interleaved[at * 2u]) / 32768.0f;
        right[at] = static_cast<float>(interleaved[at * 2u + 1u]) / 32768.0f;
    }
    write_ += accepted;
    return accepted;
}

AudioFrameIndex64 AudioResampler::pull(float* interleaved, AudioFrameIndex64 max_frames) noexcept {
    if (!configured()) return 0u;
    const AudioFrameIndex64 half = half_taps_;
    const AudioFrameIndex64 taps = taps_;
    AudioFrameIndex64 produced = 0;
    while (produced < max_frames && center_ + half < write_) {
        if (interleaved != nullptr) {
            const u64 scaled = phase_ * kPhases;
            const u64 row = scaled / up_;
            const float weight = static_cast<float>(static_cast<double>(scaled - row * up_) /
                                                    static_cast<double>(up_));
            const float* const base = base_.data() + static_cast<std::size_t>(row * taps);
            const float* const slope = slope_.data() + static_cast<std::size_t>(row * taps);
            const float* const left = left_.data() + static_cast<std::size_t>(center_ + 1u - half);
            const float* const right = right_.data() + static_cast<std::size_t>(center_ + 1u - half);
#if SHADOW_AUDIO_SSE2
            const __m128 t = _mm_set1_ps(weight);
            __m128 sum_left = _mm_setzero_ps();
            __m128 sum_right = _mm_setzero_ps();
            for (AudioFrameIndex64 tap = 0; tap < taps; tap += 4u) {
                const std::size_t at = static_cast<std::size_t>(tap);
                const __m128 coefficient =
                    _mm_add_ps(_mm_loadu_ps(base + at), _mm_mul_ps(t, _mm_loadu_ps(slope + at)));
                sum_left = _mm_add_ps(sum_left, _mm_mul_ps(coefficient, _mm_loadu_ps(left + at)));
                sum_right = _mm_add_ps(sum_right, _mm_mul_ps(coefficient, _mm_loadu_ps(right + at)));
            }
#else
            // The SSE2 kernel lane by lane: same products, same per-lane sums (no contraction,
            // -ffp-contract=off), same final order in horizontal_sum.
            float sum_left[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float sum_right[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (AudioFrameIndex64 tap = 0; tap < taps; tap += 4u) {
                const std::size_t at = static_cast<std::size_t>(tap);
                for (std::size_t lane = 0; lane < 4u; ++lane) {
                    const float coefficient = base[at + lane] + weight * slope[at + lane];
                    sum_left[lane] = sum_left[lane] + coefficient * left[at + lane];
                    sum_right[lane] = sum_right[lane] + coefficient * right[at + lane];
                }
            }
#endif
            interleaved[static_cast<std::size_t>(produced) * 2u] = horizontal_sum(sum_left);
            interleaved[static_cast<std::size_t>(produced) * 2u + 1u] = horizontal_sum(sum_right);
        }
        ++produced;
        phase_ += step();
        while (phase_ >= up_) {
            phase_ -= up_;
            ++center_;
        }
    }
    return produced;
}

double AudioResampler::response(double hz) const {
    if (!configured()) return 0.0;
    const double cycles = hz / source_hz_;  // per input frame
    double re = 0.0;
    double im = 0.0;
    for (u32 row = 0; row < kPhases; ++row) {
        for (u32 tap = 0; tap < taps_; ++tap) {
            // Fine-grid position of this coefficient in 1/kPhases input frames.
            const double n = static_cast<double>(
                (static_cast<s64>(tap) - static_cast<s64>(half_taps_) + 1) * static_cast<s64>(kPhases) -
                static_cast<s64>(row));
            const double angle = -2.0 * kPi * cycles * n / kPhases;
            const double value = base_[static_cast<std::size_t>(row) * taps_ + tap];
            re += value * std::cos(angle);
            im += value * std::sin(angle);
        }
    }
    // Linear interpolation between phases is a triangle of width 2/kPhases.
    const double u = cycles / kPhases;
    const double sinc = std::fabs(u) < 1e-15 ? 1.0 : std::sin(kPi * u) / (kPi * u);
    return std::sqrt(re * re + im * im) / kPhases * sinc * sinc;
}

// ---------------------------------------------------------------------------
// AudioRuntime state
// ---------------------------------------------------------------------------

#if defined(_WIN32)
namespace {
struct Endpoint;
}
#endif

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // alignas padding: the padding is the point
#endif

struct AudioRuntime::Impl {
    // ---- game thread -------------------------------------------------------
    // Everything publish_* touches, starting on its own cache line. The hot
    // fields fit the first line; the consumer's tail store never lands here.
    struct alignas(64) Producer {
        s16* ring = nullptr;  // null: counting only
        u64 mask = 0;
        u64 capacity = 0;
        std::atomic<u64> head{0};  // frames ever accepted, monotonic
        std::atomic<u64> blocks{0};
        std::atomic<u64> max_fill{0};
        u32 carry_bytes = 0;
        u8 carry[4] = {};
        std::atomic<u64> silence{0};
        std::atomic<u64> dropped_frames{0};
        std::atomic<u64> dropped_events{0};
        // Written before epoch, so a consumer that sees the new epoch sees
        // this reset's position (or a newer one).
        std::atomic<u64> reset_position{0};
        std::atomic<u64> epoch{0};
        // Cold: contract edges only (reset_timeline, publish_silence).
        std::atomic<u64> reset_partial_bytes{0};
        std::atomic<u64> silence_split{0};
    } producer;

    // ---- consumer thread ---------------------------------------------------
    struct alignas(64) ConsumerShared {
        std::atomic<u64> tail{0};  // frames ever removed (read or discarded), monotonic
    } consumer;

    enum Counter : std::size_t {
        kConsumed,
        kResetDropped,
        kEpochsHonoured,
        kGapDiscarded,
        kOutput,
        kUnderrunEvents,
        kUnderrunFrames,
        kPrimeSilence,
        kClipped,
        kPrimeFrames,
        kStartFill,
        kStarts,
        kWakes,
        kWakeTimeouts,
        kLateWakes,
        kMaxWakeIntervalUs,
        kMaxRenderUs,
        kDeviceStartUs,
        kFirstWakeUs,
        kReopens,
        kOpenFailures,
        kInvalidations,
        kDefaultChanges,
        kRenderErrors,
        kTrailingSilence,         // AU-T1: device frames of silence after finish_production()
        kUnderrunRecordsDropped,  // AU-T1: records lost to a full record ring
        kCounterCount,
    };
    // Written by the consumer only; relaxed load+store is a plain move.
    std::array<std::atomic<u64>, kCounterCount> counters{};
    std::array<std::atomic<u64>, Report::kFillBuckets> fill_histogram{};

    // ---- AU-T1: one record per underrun event -------------------------------
    // The consumer fills a record while a gap is open and publishes it when
    // audio resumes (only then is the gap's length known); the owner / game
    // thread prints it (publish_block, finish_production, close), so the
    // device thread never touches stderr. One writer (the consumer), one
    // reader (the owner thread).
    struct UnderrunRecord {
        u64 source_frame = 0;         // source frames ever consumed when the gap began (the audio timeline)
        u64 device_frames = 0;        // device frames of silence
        u64 queue_before_frames = 0;  // ring fill (source frames) at the wake the gap began
        u64 output_before = 0;        // device frames rendered before that wake
        u32 device_rate = 0;
        bool open_at_close = false;   // no audio followed it before close()
    };
    static constexpr std::size_t kUnderrunRecords = 64u;
    std::array<UnderrunRecord, kUnderrunRecords> underrun_records{};
    std::atomic<u64> underrun_records_written{0};
    std::atomic<u64> underrun_records_read{0};
    // Consumer-private: the open gap.
    bool record_open = false;
    UnderrunRecord open_record{};
    // Set by finish_production(): the game produces nothing more, so silence
    // from then on is the end of the run, not a gap.
    std::atomic<bool> production_finished{false};

    void commit_underrun_record() noexcept {
        if (!record_open) return;
        record_open = false;
        const u64 written = underrun_records_written.load(std::memory_order_relaxed);
        if (written - underrun_records_read.load(std::memory_order_acquire) >= kUnderrunRecords) {
            add(kUnderrunRecordsDropped, 1u);
            return;
        }
        underrun_records[static_cast<std::size_t>(written % kUnderrunRecords)] = open_record;
        underrun_records_written.store(written + 1u, std::memory_order_release);
    }
    void print_underrun_records() noexcept;

    // Consumer-private.
    // SHADOW_AUDIO_RATE_TRIM=1 (default off; docs/TIMING_ARCHITECTURE.md 7): the resampler's ratio follows the ring's
    // fill, so that a game clock running a few ppm off the device (a display-locked game clock) neither fills nor
    // drains the ring. The target is the fill the ring settles at on its own (learned over the first seconds of
    // playback), so the latency stays what it is today. Off: the resampler is configured exactly as before.
    struct RateTrim {
        bool enabled = false;
        u64 learn_wakes = 0;     // wakes seen so far (the first kSettleWakes are not averaged)
        double learn_sum = 0.0;
        double target = 0.0;     // source frames
        double smoothed = 0.0;   // fill, exponentially smoothed (~1 s at 10 ms wakes)
        double ppm = 0.0;        // the trim applied
        double units_per_ppm = 0.0;
        std::chrono::steady_clock::time_point traced{};
        // The fill right after the start still holds the prime (measured 2026-10-31: ~1,350 frames for the first
        // seconds, ~1,000 once settled); a target learned there raised the steady fill by ~11 ms. So: let 20 s pass,
        // then average 10 s.
        static constexpr u64 kSettleWakes = 2000;        // ~20 s at 10 ms wakes
        static constexpr u64 kLearnWakes = 3000;         // settle + ~10 s averaged
        static constexpr double kAlpha = 0.01;
        static constexpr double kPpmPerFrame = 0.5;      // 10 ppm costs 20 frames (0.6 ms) of steady offset
        static constexpr double kMaxPpm = 300.0;
    } rate_trim;
    void update_rate_trim(SampleCounter64 fill) noexcept;
    SampleCounter64 tail = 0;
    u64 seen_epoch = 0;
    bool running = false;  // past the prime
    bool holding = false;  // render prime silence whatever the fill (endpoint start-up)
    bool in_underrun = false;
    AudioFrameIndex64 prime_frames = 0;
    AudioDeviceFormat format{};
    AudioResampler resampler;
    shadow::memory::Vector<s16, shadow::memory::Domain::Audio> feed;
    shadow::memory::Vector<float, shadow::memory::Domain::Audio> scratch;
    AudioFrameIndex64 fade_frames = 0;
    AudioFrameIndex64 ramp_pos = 0;
    AudioFrameIndex64 decay_pos = 0;
    bool in_gap = true;
    float last[2] = {};
    float decay_from[2] = {};

    // ---- owner / any thread --------------------------------------------------
    Config config{};
    shadow::memory::Vector<s16, shadow::memory::Domain::Audio> ring_storage;
    std::atomic<Mode> mode{Mode::Null};
    std::atomic<bool> device_open{false};
    std::atomic<bool> device_started{false};
    std::atomic<bool> open_pending_at_return{false};
    // ES-W7: open() with Config::defer_handshake left the first-open wait to
    // wait_open(); the deadline is open_timeout_ms after open().
    bool handshake_deferred = false;
    std::chrono::steady_clock::time_point handshake_deadline{};

    // Endpoint description and errors. The consumer writes it on open and on
    // errors; report() reads it. The producer never takes this lock.
    mutable std::mutex info_mutex;
    std::string device_name;
    std::string device_format;
    std::string last_error;
    AudioFrameIndex64 ring_capacity = 0;
    u32 device_rate = 0;
    u32 device_channels = 0;
    AudioFrameIndex64 period_frames = 0;
    AudioFrameIndex64 buffer_frames = 0;
    bool low_latency = false;
    bool mmcss = false;
    u64 resampler_up = 0;
    u64 resampler_down = 0;
    u32 resampler_taps = 0;

#if defined(_WIN32)
    std::thread consumer_thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> default_changed{false};
    HANDLE control_event = nullptr;
    HANDLE audio_event = nullptr;
    std::mutex handshake_mutex;
    std::condition_variable handshake_cv;
    int open_result = -1;         // -1 pending, 0 failed, 1 opened
    bool open_abandoned = false;  // open() returned at its timeout: nobody acts on open_result
    u32 consecutive_errors = 0;
    std::chrono::steady_clock::time_point started_at{};
    bool awaiting_first_wake = false;
    u32 fault_opens_failed = 0;  // Config::fault_failed_opens already spent
    u64 wakes_at_open = 0;       // kWakes when the current endpoint opened
#endif

    void add(Counter counter, u64 amount) noexcept {
        counters[counter].store(counters[counter].load(std::memory_order_relaxed) + amount,
                                std::memory_order_relaxed);
    }
    void raise(Counter counter, u64 value) noexcept {
        if (value > counters[counter].load(std::memory_order_relaxed)) {
            counters[counter].store(value, std::memory_order_relaxed);
        }
    }
    u64 get(Counter counter) const noexcept {
        return counters[counter].load(std::memory_order_relaxed);
    }

    void set_error(std::string text) {
        std::lock_guard<std::mutex> lock(info_mutex);
        last_error = std::move(text);
    }

    // Ring fill as a value that existed, from any thread. Tail is loaded first:
    // it never passes the head, so the difference cannot wrap. If the tail did
    // not move across the head load, the pair coexisted and the fill is within
    // the capacity; the consumer moves it ~100 times a second, so a retry is rare.
    u64 snapshot_fill() const noexcept {
        u64 tail_value = consumer.tail.load(std::memory_order_relaxed);
        for (int attempt = 0;; ++attempt) {
            const u64 head = producer.head.load(std::memory_order_relaxed);
            const u64 again = consumer.tail.load(std::memory_order_relaxed);
            if (again == tail_value || attempt == 16) return head - tail_value;
            tail_value = again;
        }
    }

    void clear();
    void release_ring() noexcept;
    bool configure_consumer(const AudioDeviceFormat& device, AudioFrameIndex64 device_period_frames);
    void sync_epoch() noexcept;
    AudioFrameIndex64 read_ring(s16* out, AudioFrameIndex64 max_frames) noexcept;
    void discard_excess(AudioFrameIndex64 keep_frames) noexcept;
    void render(u8* out, AudioFrameIndex64 frames) noexcept;
    void convert(const float* stereo, AudioFrameIndex64 frames, u8* out) noexcept;
    void fade_in(float* stereo, AudioFrameIndex64 frames) noexcept;
    void emit_silence(u8* out, AudioFrameIndex64 frames) noexcept;

#if defined(_WIN32)
    void consumer_main();
    bool finish_handshake(bool opened);
    bool open_endpoint(IMMDeviceEnumerator* enumerator, Endpoint& endpoint);
    void close_endpoint(Endpoint& endpoint) noexcept;
    bool start_endpoint(Endpoint& endpoint);
    bool render_endpoint(Endpoint& endpoint);
    bool endpoint_failed(Endpoint& endpoint, HRESULT hr, const char* what);
#endif
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

void AudioRuntime::Impl::clear() {
    producer.ring = nullptr;
    producer.mask = 0;
    producer.capacity = 0;
    producer.head.store(0);
    producer.blocks.store(0);
    producer.max_fill.store(0);
    producer.carry_bytes = 0;
    producer.silence.store(0);
    producer.dropped_frames.store(0);
    producer.dropped_events.store(0);
    producer.reset_position.store(0);
    producer.epoch.store(0);
    producer.reset_partial_bytes.store(0);
    producer.silence_split.store(0);
    consumer.tail.store(0);
    for (auto& counter : counters) counter.store(0);
    for (auto& bucket : fill_histogram) bucket.store(0);
    tail = 0;
    seen_epoch = 0;
    running = false;
    holding = false;
    in_underrun = false;
    record_open = false;
    open_record = UnderrunRecord{};
    underrun_records_written.store(0);
    underrun_records_read.store(0);
    production_finished.store(false);
    prime_frames = 0;
    device_open.store(false);
    device_started.store(false);
    open_pending_at_return.store(false);
    handshake_deferred = false;
    std::lock_guard<std::mutex> lock(info_mutex);
    device_name.clear();
    device_format.clear();
    last_error.clear();
    ring_capacity = 0;
    device_rate = 0;
    device_channels = 0;
    period_frames = 0;
    buffer_frames = 0;
    low_latency = false;
    mmcss = false;
    resampler_up = 0;
    resampler_down = 0;
    resampler_taps = 0;
}

void AudioRuntime::Impl::release_ring() noexcept {
    // Paired with the ALLOC in open(). close() always runs release_ring() before
    // open() reassigns, so the FREE precedes the next ALLOC; the address comes
    // from producer.ring rather than a re-read of the Vector because a
    // mismatched pair is a hard error in Tracy's memory view. The guard covers
    // the paths that release a ring that was never taken (Null mode, a failed
    // allocation, a second close()).
    if (producer.ring != nullptr) GEKKO_PROFILE_FREE(producer.ring, "audio-ring");
    producer.ring = nullptr;
    producer.capacity = 0;
    producer.mask = 0;
    ring_storage = shadow::memory::Vector<s16, shadow::memory::Domain::Audio>();
    std::lock_guard<std::mutex> lock(info_mutex);
    ring_capacity = 0;
}

bool AudioRuntime::Impl::configure_consumer(const AudioDeviceFormat& device,
                                            AudioFrameIndex64 device_period_frames) {
    if (device.rate == 0u || device.channels == 0u || device.bytes_per_sample() == 0u) {
        set_error("unsupported device format");
        return false;
    }
    rate_trim = RateTrim{};
    {
        // Default ON since 2026-11-04 (with the phase lock: a 12-minute run kept the ring within its noise, 0 underruns;
        // docs/PERF_60FPS_2026-11-04.md). SHADOW_AUDIO_RATE_TRIM=0 or --no-audio-rate-trim: the fixed ratio as before.
        const char* v = std::getenv("SHADOW_AUDIO_RATE_TRIM");
        rate_trim.enabled = !(v != nullptr && v[0] == '0');
    }
    if (!resampler.configure(kAudioSourceRateNum, kAudioSourceRateDen, device.rate, rate_trim.enabled)) {
        set_error("resampler rejected the device rate");
        return false;
    }
    // The largest allocation this subsystem owns, and the only one whose size
    // the DEVICE decides: base_ and slope_ are kPhases x taps floats each,
    // left_ and right_ taps + kHistoryFrames each. ~193 KiB at the default 80
    // taps, ~4.2 MiB at the 2048-tap downsampling limit - a 22x swing that
    // nothing reported. A plot and not an ALLOC/FREE pair because configure()
    // reassigns the vectors with no hook before the old buffers die, so a pair
    // here would be mis-matched.
    GEKKO_PROFILE_PLOT(
        "aud/resampler-bytes",
        static_cast<double>(
            (static_cast<u64>(AudioResampler::kPhases) * resampler.taps() * 2u +
             (static_cast<u64>(resampler.taps()) + AudioResampler::kHistoryFrames) * 2u) *
            sizeof(float)));
    format = device;
    const AudioFrameIndex64 period_source = source_frames_ceil(
        static_cast<u64>(config.prime_device_periods) * device_period_frames, device.rate);
    prime_frames = std::min<AudioFrameIndex64>(
        period_source + source_frames_ceil(config.prime_extra_ms, 1000u), producer.capacity);
    counters[kPrimeFrames].store(prime_frames, std::memory_order_relaxed);
    fade_frames = static_cast<AudioFrameIndex64>(config.gap_fade_ms) * device.rate / 1000u;
    ramp_pos = 0;
    decay_pos = fade_frames;  // nothing has played: no tail to fade
    in_gap = true;
    last[0] = last[1] = 0.0f;
    decay_from[0] = decay_from[1] = 0.0f;
    running = false;
    in_underrun = false;
    std::lock_guard<std::mutex> lock(info_mutex);
    device_rate = device.rate;
    device_channels = device.channels;
    period_frames = device_period_frames;
    device_format = kind_name(device);
    resampler_up = resampler.up();
    resampler_down = resampler.down();
    resampler_taps = resampler.taps();
    return true;
}

void AudioRuntime::Impl::sync_epoch() noexcept {
    u64 epoch = producer.epoch.load(std::memory_order_acquire);
    if (epoch == seen_epoch) return;
    u64 position = 0;
    for (;;) {
        epoch = producer.epoch.load(std::memory_order_acquire);
        // A concurrent next reset may publish its boundary before its epoch.
        // Acquire that boundary independently: head stores before it then
        // happen before subsequent head reads, so tail cannot outrun a stale
        // observed head. Resets may coalesce; this is not an exact epoch pair.
        position = producer.reset_position.load(std::memory_order_acquire);
        if (producer.epoch.load(std::memory_order_acquire) == epoch) break;
    }
    // Emitted here, before `tail = position` below: after that assignment the
    // delta is zero and the marker would always claim nothing was dropped. A
    // savestate load is the documented origin of permanent audio latency, so
    // every queue reading after one has to be read relative to this marker.
    // Guarded because the formatting exists only for the capture; the epoch
    // early-returns above keep this to one message per reset_timeline.
    if (GEKKO_PROFILE_CONNECTED()) {
        char text[96];
        const int length = std::snprintf(
            text, sizeof(text), "aud/timeline-reset epoch=%llu dropped=%llu frames",
            static_cast<unsigned long long>(epoch),
            static_cast<unsigned long long>(position > tail ? position - tail : 0u));
        if (length > 0) {
            GEKKO_PROFILE_MESSAGE(text, static_cast<std::size_t>(length) < sizeof(text)
                                            ? static_cast<std::size_t>(length)
                                            : sizeof(text) - 1u);
        }
    }
    if (position > tail) {
        add(kResetDropped, position - tail);
        tail = position;
        consumer.tail.store(tail, std::memory_order_release);
    }
    add(kEpochsHonoured, 1u);
    seen_epoch = epoch;
    if (resampler.configured()) resampler.reset();
    in_underrun = false;
    commit_underrun_record();  // a gap open across a timeline reset ends there
    if (config.reprime_on_reset) running = false;
}

AudioFrameIndex64 AudioRuntime::Impl::read_ring(s16* out, AudioFrameIndex64 max_frames) noexcept {
    // Acquire the published prefix before reading PCM. Release its ownership
    // only after copying it, so the producer cannot overwrite a live read.
    const SampleCounter64 available = producer.head.load(std::memory_order_acquire) - tail;
    const AudioFrameIndex64 frames = std::min<AudioFrameIndex64>(available, max_frames);
    if (frames == 0u) return 0u;
    const AudioFrameIndex64 index = tail & producer.mask;
    const AudioFrameIndex64 first = std::min<AudioFrameIndex64>(frames, producer.capacity - index);
    std::memcpy(out, producer.ring + static_cast<std::size_t>(index) * 2u,
                static_cast<std::size_t>(first) * 2u * sizeof(s16));
    std::memcpy(out + static_cast<std::size_t>(first) * 2u, producer.ring,
                static_cast<std::size_t>(frames - first) * 2u * sizeof(s16));
    tail += frames;
    consumer.tail.store(tail, std::memory_order_release);
    add(kConsumed, frames);
    return frames;
}

void AudioRuntime::Impl::discard_excess(AudioFrameIndex64 keep_frames) noexcept {
    sync_epoch();
    const u64 fill = producer.head.load(std::memory_order_acquire) - tail;
    if (fill <= keep_frames) return;
    add(kGapDiscarded, fill - keep_frames);
    // The subsystem's eviction signal: audio thrown out of the ring because no
    // endpoint could play it (the drain timer, and the trim after every open).
    // Without it a reopen is indistinguishable from a production hole on the
    // queue-depth plots - the ring empties in both cases, but only one of them
    // is the guest's fault. The early return above keeps it free otherwise.
    GEKKO_PROFILE_PLOT("aud/gap-discarded", static_cast<double>(get(kGapDiscarded)));
    tail += fill - keep_frames;
    consumer.tail.store(tail, std::memory_order_release);
}

void AudioRuntime::Impl::fade_in(float* stereo, AudioFrameIndex64 frames) noexcept {
    AudioFrameIndex64 i = 0;
    while (ramp_pos < fade_frames && i < frames) {
        const float gain = static_cast<float>(ramp_pos + 1u) / static_cast<float>(fade_frames + 1u);
        stereo[static_cast<std::size_t>(i) * 2u] *= gain;
        stereo[static_cast<std::size_t>(i) * 2u + 1u] *= gain;
        ++ramp_pos;
        ++i;
    }
    last[0] = stereo[static_cast<std::size_t>(frames - 1u) * 2u];
    last[1] = stereo[static_cast<std::size_t>(frames - 1u) * 2u + 1u];
    in_gap = false;
}

void AudioRuntime::Impl::emit_silence(u8* out, AudioFrameIndex64 frames) noexcept {
    if (!in_gap) {
        // Ramp the last played value to zero instead of stepping to it.
        in_gap = true;
        decay_pos = 0;
        decay_from[0] = last[0];
        decay_from[1] = last[1];
    }
    ramp_pos = 0;  // whatever plays next fades in
    const AudioByteOffset64 frame_bytes = format.bytes_per_frame();
    AudioFrameIndex64 done = 0;
    while (done < frames && decay_pos < fade_frames) {
        const AudioFrameIndex64 run =
            std::min({frames - done, kScratchFrames, fade_frames - decay_pos});
        for (AudioFrameIndex64 i = 0; i < run; ++i) {
            const float gain = static_cast<float>(fade_frames - decay_pos) / static_cast<float>(fade_frames + 1u);
            scratch[static_cast<std::size_t>(i) * 2u] = decay_from[0] * gain;
            scratch[static_cast<std::size_t>(i) * 2u + 1u] = decay_from[1] * gain;
            ++decay_pos;
        }
        convert(scratch.data(), run, out + static_cast<std::size_t>(done * frame_bytes));
        done += run;
    }
    // Every supported kind encodes zero as all-zero bytes.
    std::memset(out + static_cast<std::size_t>(done * frame_bytes), 0,
                static_cast<std::size_t>((frames - done) * frame_bytes));
}

namespace {
// The player's master volume (Options > Audio, gekko/pc_settings.h): a linear gain on the endpoint stream, after
// the game's mix, so the game's PCM is never touched. 1 = unity: the multiply is skipped (bit-identical output).
std::atomic<float> g_output_gain{1.0f};
}  // namespace

void set_audio_output_gain(float gain) noexcept {
    g_output_gain.store(std::clamp(gain, 0.0f, 1.0f), std::memory_order_relaxed);
}

float audio_output_gain() noexcept { return g_output_gain.load(std::memory_order_relaxed); }

void AudioRuntime::Impl::convert(const float* stereo, AudioFrameIndex64 frames, u8* out) noexcept {
    const u32 channels = format.channels;
    const u32 sample_bytes = format.bytes_per_sample();
    const AudioSampleKind kind = format.kind;
    const u32 valid_bits = std::clamp(format.valid_bits, 8u, 32u);
    u64 clipped = 0;
    const auto write = [&](u8* dst, float x) noexcept {
        switch (kind) {
            case AudioSampleKind::Float32:
                std::memcpy(dst, &x, 4u);
                break;
            case AudioSampleKind::Float64: {
                const double value = x;
                std::memcpy(dst, &value, 8u);
                break;
            }
            case AudioSampleKind::Int16: {
                const s16 value = static_cast<s16>(std::min<long long>(std::llrint(x * 32768.0), 32767));
                std::memcpy(dst, &value, 2u);
                break;
            }
            case AudioSampleKind::Int24: {
                const s32 value = static_cast<s32>(std::min<long long>(std::llrint(x * 8388608.0), 8388607));
                dst[0] = static_cast<u8>(value);
                dst[1] = static_cast<u8>(value >> 8);
                dst[2] = static_cast<u8>(value >> 16);
                break;
            }
            case AudioSampleKind::Int32: {
                const long long top = (1ll << (valid_bits - 1u)) - 1;
                const long long value = std::min<long long>(
                    std::llrint(static_cast<double>(x) * static_cast<double>(1ll << (valid_bits - 1u))), top);
                // Left-justified in the 32-bit container.
                const u32 word = static_cast<u32>(static_cast<s32>(value)) << (32u - valid_bits);
                std::memcpy(dst, &word, 4u);
                break;
            }
        }
    };
    const auto clamp = [&](float x) noexcept {
        if (x > 1.0f) {
            ++clipped;
            return 1.0f;
        }
        if (x < -1.0f) {
            ++clipped;
            return -1.0f;
        }
        return x;
    };
    const AudioByteOffset64 frame_bytes =
        static_cast<AudioByteOffset64>(channels) * sample_bytes;
    // Kept separate from aud/resample because Int24's byte-at-a-time path is a
    // different shape from a Float32 memcpy: this is the one place where a bad
    // endpoint format shows up as host CPU. Per chunk (from render) or per gap
    // fade run (from emit_silence), never per frame.
    GEKKO_PROFILE_ZONE("aud/convert");
    GEKKO_PROFILE_VALUE(frames);
    // The master volume, ramped across the chunk from the last chunk's gain (no zipper noise); unity skips it.
    static float last_gain = 1.0f;  // the consumer thread's own (convert runs on it alone)
    const float target_gain = g_output_gain.load(std::memory_order_relaxed);
    const bool unity = target_gain == 1.0f && last_gain == 1.0f;
    const float gain_step = frames > 0 ? (target_gain - last_gain) / static_cast<float>(frames) : 0.0f;
    for (AudioFrameIndex64 i = 0; i < frames; ++i) {
        const std::size_t at = static_cast<std::size_t>(i);
        const float gain = unity ? 1.0f : last_gain + gain_step * static_cast<float>(i + 1);
        const float left = clamp(unity ? stereo[at * 2u] : stereo[at * 2u] * gain);
        const float right = clamp(unity ? stereo[at * 2u + 1u] : stereo[at * 2u + 1u] * gain);
        u8* const frame = out + static_cast<std::size_t>(i * frame_bytes);
        if (channels == 1u) {
            write(frame, 0.5f * (left + right));
            continue;
        }
        write(frame, left);
        write(frame + sample_bytes, right);
        if (channels > 2u) std::memset(frame + 2u * sample_bytes, 0, (channels - 2u) * sample_bytes);
    }
    if (frames > 0) last_gain = target_gain;
    if (clipped != 0u) add(kClipped, clipped);
}

void AudioRuntime::Impl::update_rate_trim(SampleCounter64 fill) noexcept {
    RateTrim& t = rate_trim;
    const double f = static_cast<double>(fill);
    if (t.learn_wakes < RateTrim::kLearnWakes) {
        if (t.learn_wakes >= RateTrim::kSettleWakes) t.learn_sum += f;
        if (++t.learn_wakes == RateTrim::kLearnWakes) {
            t.target = t.learn_sum / static_cast<double>(RateTrim::kLearnWakes - RateTrim::kSettleWakes);
            t.smoothed = t.target;
            t.units_per_ppm = static_cast<double>(resampler.down()) * 1e-6;
        }
        return;
    }
    t.smoothed += RateTrim::kAlpha * (f - t.smoothed);
    // Fuller than the target: consume faster (more input per output frame).
    t.ppm = std::clamp((t.smoothed - t.target) * RateTrim::kPpmPerFrame, -RateTrim::kMaxPpm, RateTrim::kMaxPpm);
    resampler.set_trim(static_cast<std::int64_t>(std::llround(t.ppm * t.units_per_ppm)));
    const auto now = std::chrono::steady_clock::now();
    if (now - t.traced >= std::chrono::seconds(1)) {
        t.traced = now;
        shadow::render::present_trace::record(shadow::render::present_trace::Ev::AudioTrim,
            static_cast<std::uint64_t>(std::llround(t.ppm * 1000.0)), static_cast<std::uint64_t>(std::llround(t.target)),
            static_cast<std::uint64_t>(std::llround(t.smoothed)));
    }
}

void AudioRuntime::Impl::render(u8* out, AudioFrameIndex64 frames) noexcept {
    // One site covers both consumers: AudioRuntime::render (Manual mode)
    // delegates here too. This is the zone that will be open next to the
    // queue-depth plots at the instant of a starvation, and it deliberately
    // covers the prime-silence early return below.
    GEKKO_PROFILE_ZONE("aud/mix");
    GEKKO_PROFILE_VALUE(frames);
    add(kWakes, 1u);
    sync_epoch();
    const SampleCounter64 fill = producer.head.load(std::memory_order_relaxed) - tail;
    // The histogram below computes exactly this quantity and throws the time
    // axis away: it says a run spent N wakes near empty but never WHEN, which
    // is why the 2026-09 starvation could not be tied to a cause. Kept separate
    // from aud/queue-ms-produce on purpose - producer-after-write minus
    // consumer-before-read is the in-flight window, and the two series
    // diverging is itself the signal.
    GEKKO_PROFILE_PLOT("aud/queue-ms-consume",
                       1000.0 * static_cast<double>(fill) *
                           static_cast<double>(kAudioSourceRateDen) /
                           static_cast<double>(kAudioSourceRateNum));
    const u64 bucket = fill * kAudioSourceRateDen * 1000u / (kAudioSourceRateNum * Report::kFillBucketMs);
    auto& slot = fill_histogram[static_cast<std::size_t>(std::min<u64>(bucket, Report::kFillBuckets - 1u))];
    slot.store(slot.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);

    // gekko/schedule_offset.h: a lead the game runs ahead with adds that much audio to the queue. The prime and the
    // trim work on the NEUTRAL queue (fill minus the lead), so a prime during a lead, or a trim target learned during
    // one, does not leave the queue short once the lead is given back.
    const u64 lead_frames = gekko::schedule_offset::applied_lead_frames(kAudioSourceRateNum, kAudioSourceRateDen);
    const u64 neutral_fill = fill > lead_frames ? fill - lead_frames : 0u;
    gekko::schedule_offset::audio_queued_ns().store(
        static_cast<std::int64_t>(fill * kAudioSourceRateDen * 1'000'000'000ull / kAudioSourceRateNum),
        std::memory_order_relaxed);
    if (running && rate_trim.enabled) update_rate_trim(neutral_fill);

    const auto feed_resampler = [this](AudioFrameIndex64 need) noexcept {
        while (need != 0u) {
            const AudioFrameIndex64 take = std::min({need, resampler.input_room(), kFeedFrames});
            if (take == 0u) break;
            const AudioFrameIndex64 got = read_ring(feed.data(), take);
            if (got == 0u) break;
            const AudioFrameIndex64 accepted = resampler.push(feed.data(), got);
            if (accepted == 0u) break;
            need -= accepted;
            if (accepted < got) break;
        }
    };

    if (!running) {
        if (holding || neutral_fill < prime_frames) {
            emit_silence(out, frames);
            add(kPrimeSilence, frames);
            add(kOutput, frames);
            // Sampled at this exit as well as the end of the function, so there
            // is one sample per wake. Without it the series carries no samples
            // through the whole prime and through a held endpoint start (the
            // measured 181-187 ms IAudioClient::Start block), and a reader
            // cannot tell that flat stretch from a stall - the exact misreading
            // this plot exists to prevent.
            GEKKO_PROFILE_PLOT("aud/frames-consumed", static_cast<double>(get(kConsumed)));
            return;
        }
        // Prefill look-ahead taps from the already-primed ring so the first
        // audible period is a full device buffer, not a counted underrun.
        feed_resampler(resampler.input_needed(frames));
        running = true;
        in_underrun = false;
        add(kStarts, 1u);
        counters[kStartFill].store(fill, std::memory_order_relaxed);
    }

    const AudioByteOffset64 frame_bytes = format.bytes_per_frame();
    AudioFrameIndex64 produced = 0;
    while (produced < frames) {
        const AudioFrameIndex64 chunk = std::min(frames - produced, kScratchFrames);
        // Per chunk, which is 1-2 iterations per wake: a zone inside
        // AudioResampler::pull would be per output frame over an 80-2048-tap
        // SSE loop, tens of thousands a second. aud/convert nests inside this
        // one, so the resampling cost is this zone's self time.
        GEKKO_PROFILE_ZONE("aud/resample");
        GEKKO_PROFILE_VALUE(chunk);
        // Move exactly what this chunk needs: the ring keeps the rest, so its
        // fill stays the honest measure of queued audio.
        feed_resampler(resampler.input_needed(chunk));
        const AudioFrameIndex64 made = resampler.pull(scratch.data(), chunk);
        if (made != 0u) {
            fade_in(scratch.data(), made);
            convert(scratch.data(), made, out + static_cast<std::size_t>(produced * frame_bytes));
            produced += made;
        }
        if (made < chunk) break;
    }
    // Any audio played ends the previous gap. A producer running steadily
    // below real time plays some audio and then runs short on every wake, and
    // each of those shortfalls is a separate audible gap.
    if (produced != 0u) {
        commit_underrun_record();  // AU-T1: the gap's length is known now
        in_underrun = false;
    }
    if (produced < frames) {
        // Underrun: play silence for exactly the missing frames and keep going.
        // No pause, no reprime; the next frames continue the same timeline.
        const AudioFrameIndex64 missing = frames - produced;
        if (production_finished.load(std::memory_order_acquire)) {
            // AU-T1: the game has stopped producing (finish_production), so this
            // is the end of the run, not an audible gap during play. A gap that
            // was already open is recorded as it stood.
            commit_underrun_record();
            add(kTrailingSilence, missing);
        } else {
            if (!in_underrun) {
                in_underrun = true;
                add(kUnderrunEvents, 1u);
                // Not one marker per contiguous starvation: the line above clears
                // in_underrun on any audio at all, so a producer running steadily
                // below real time opens a new event on every wake. Expect this row
                // to be dense during a chronic shortfall - that density IS the
                // finding, and no other budget here is sized against it.
                GEKKO_PROFILE_MESSAGE_LITERAL("aud/underrun");
                record_open = true;
                open_record = UnderrunRecord{};
                open_record.source_frame = tail;
                open_record.queue_before_frames = fill;
                open_record.output_before = get(kOutput);
                open_record.device_rate = format.rate;
            }
            add(kUnderrunFrames, missing);
            open_record.device_frames += missing;
            // Cumulative and stepped, so the height of each step is the length of
            // the audible gap in device frames at the instant it happened. The old
            // path's underrun counter read 0 through a whole mission of real
            // starvations (docs/audio-2026-09-16 D1); dating every increment on the
            // same axis as the zone that caused it is the fix.
            GEKKO_PROFILE_PLOT("aud/underrun-frames", static_cast<double>(get(kUnderrunFrames)));
        }
        emit_silence(out + static_cast<std::size_t>(produced * frame_bytes), missing);
    }
    add(kOutput, frames);
    GEKKO_PROFILE_PLOT("aud/frames-consumed", static_cast<double>(get(kConsumed)));
}

// ---------------------------------------------------------------------------
// WASAPI consumer
// ---------------------------------------------------------------------------

#if defined(_WIN32)
namespace {

constexpr GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
// PKEY_Device_FriendlyName, spelled out so no INITGUID translation unit is needed.
constexpr PROPERTYKEY kFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

struct Endpoint {
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    IAudioClock* clock = nullptr;  // only while the present trace records (the AudioClock rows)
    std::chrono::steady_clock::time_point clock_sampled{};
    std::wstring id;
    UINT32 buffer_frames = 0;
    bool started = false;
    bool open() const noexcept { return render != nullptr; }
};

template <typename T>
void release(T*& pointer) noexcept {
    if (pointer != nullptr) {
        pointer->Release();
        pointer = nullptr;
    }
}

std::string narrow(const wchar_t* text) {
    if (text == nullptr) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::string hresult_text(const char* what, HRESULT hr) {
    char text[160];
    std::snprintf(text, sizeof(text), "%s failed (hr=0x%08lX)", what, static_cast<unsigned long>(hr));
    return text;
}

bool describe_format(const WAVEFORMATEX* wave, AudioDeviceFormat& out) {
    if (wave == nullptr || wave->nChannels == 0u || wave->nSamplesPerSec == 0u) return false;
    bool is_float = wave->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool is_pcm = wave->wFormatTag == WAVE_FORMAT_PCM;
    u32 valid = wave->wBitsPerSample;
    if (wave->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wave->cbSize >= 22u) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave);
        is_float = IsEqualGUID(extensible->SubFormat, kSubtypeFloat) != 0;
        is_pcm = IsEqualGUID(extensible->SubFormat, kSubtypePcm) != 0;
        if (extensible->Samples.wValidBitsPerSample != 0u) valid = extensible->Samples.wValidBitsPerSample;
    }
    AudioDeviceFormat format;
    format.rate = wave->nSamplesPerSec;
    format.channels = wave->nChannels;
    const u32 bits = wave->wBitsPerSample;
    if (is_float && bits == 32u) {
        format.kind = AudioSampleKind::Float32;
    } else if (is_float && bits == 64u) {
        format.kind = AudioSampleKind::Float64;
    } else if (is_pcm && bits == 16u && valid == 16u) {
        format.kind = AudioSampleKind::Int16;
    } else if (is_pcm && bits == 24u && valid == 24u) {
        format.kind = AudioSampleKind::Int24;
    } else if (is_pcm && bits == 32u && (valid == 24u || valid == 32u)) {
        format.kind = AudioSampleKind::Int32;
        format.valid_bits = valid;
    } else {
        return false;
    }
    if (wave->nBlockAlign != format.bytes_per_frame()) return false;
    out = format;
    return true;
}

// Endpoint notifications arrive on a system thread. They set a flag and wake
// the consumer, nothing more: the endpoint is reopened on the consumer thread,
// and COM forbids (un)registering from inside a callback.
class EndpointNotifier final : public IMMNotificationClient {
public:
    EndpointNotifier(std::atomic<bool>& changed, HANDLE wake) : changed_(changed), wake_(wake) {}

    // Owned by the consumer thread's frame and unregistered before it ends, so
    // COM reference counts never delete it.
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { return --references_; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) {
            changed_.store(true);
            SetEvent(wake_);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    std::atomic<bool>& changed_;
    HANDLE wake_;
    std::atomic<ULONG> references_{1};
};

bool default_endpoint_differs(IMMDeviceEnumerator* enumerator, const Endpoint& endpoint) {
    IMMDevice* device = nullptr;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return true;
    bool differs = true;
    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id)) && id != nullptr) {
        differs = endpoint.id != id;
        CoTaskMemFree(id);
    }
    release(device);
    return differs;
}

}  // namespace

// Returns whether open() is still waiting and will act on the result. Both
// sides decide under the handshake mutex, so exactly one of them owns a failed
// first open: open() (falls back to Null) or the consumer (keeps retrying).
bool AudioRuntime::Impl::finish_handshake(bool opened) {
    std::lock_guard<std::mutex> lock(handshake_mutex);
    if (open_result < 0) open_result = opened ? 1 : 0;
    handshake_cv.notify_all();
    return !open_abandoned;
}

bool AudioRuntime::Impl::open_endpoint(IMMDeviceEnumerator* enumerator, Endpoint& endpoint) {
    // Per resource, 1/run normally and at most 1/s while retrying (kOpenRetry).
    // Besides the COM activation this covers configure_consumer ->
    // AudioResampler::configure, which evaluates bessel_i0 once per tap of a
    // (kPhases + 1) x taps double-precision kernel: 20,560 evaluations at the
    // default 80 taps, 526k at the 2048-tap limit. That is real work on the
    // consumer thread inside the path that delays first audio.
    GEKKO_PROFILE_ZONE("aud/open-endpoint");
    if (fault_opens_failed < config.fault_failed_opens) {
        // Before touching the device, so it behaves the same with none present.
        ++fault_opens_failed;
        set_error("injected endpoint open failure (Config::fault_failed_opens)");
        return false;
    }
    HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint.device);
    if (FAILED(hr)) {
        set_error(hresult_text("GetDefaultAudioEndpoint", hr));
        return false;
    }
    LPWSTR id = nullptr;
    if (SUCCEEDED(endpoint.device->GetId(&id)) && id != nullptr) {
        endpoint.id = id;
        CoTaskMemFree(id);
    }
    std::string name = "default";
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(endpoint.device->OpenPropertyStore(STGM_READ, &store))) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(store->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR) {
            name = narrow(value.pwszVal);
        }
        PropVariantClear(&value);
        release(store);
    }

    AudioDeviceFormat device_format_value;
    AudioFrameIndex64 period = 0;
    bool low_latency_period = false;
    WAVEFORMATEX* mix = nullptr;

    // Preferred: the engine's minimum shared period (IAudioClient3).
    IAudioClient3* client3 = nullptr;
    if (SUCCEEDED(endpoint.device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr,
                                            reinterpret_cast<void**>(&client3)))) {
        UINT32 default_period = 0;
        UINT32 fundamental = 0;
        UINT32 minimum = 0;
        UINT32 maximum = 0;
        if (SUCCEEDED(client3->GetMixFormat(&mix)) && describe_format(mix, device_format_value) &&
            SUCCEEDED(client3->GetSharedModeEnginePeriod(mix, &default_period, &fundamental, &minimum,
                                                         &maximum)) &&
            SUCCEEDED(client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minimum, mix,
                                                           nullptr))) {
            endpoint.client = client3;
            client3 = nullptr;
            period = minimum;  // UINT32 engine period, widened at the API edge
            low_latency_period = true;
        }
        release(client3);
        if (mix != nullptr) {
            CoTaskMemFree(mix);
            mix = nullptr;
        }
    }

    // Fallback: IAudioClient with a 10 ms buffer. A failed Initialize leaves a
    // client unusable, so this is a fresh activation.
    if (endpoint.client == nullptr) {
        IAudioClient* client = nullptr;
        hr = endpoint.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(&client));
        if (FAILED(hr)) {
            set_error(hresult_text("IMMDevice::Activate(IAudioClient)", hr));
            release(endpoint.device);
            return false;
        }
        hr = client->GetMixFormat(&mix);
        if (FAILED(hr)) {
            set_error(hresult_text("IAudioClient::GetMixFormat", hr));
            release(client);
            release(endpoint.device);
            return false;
        }
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        WAVEFORMATEXTENSIBLE float_format{};
        const WAVEFORMATEX* use = mix;
        if (!describe_format(mix, device_format_value)) {
            // An exotic mix format: render float32 at the mix rate and let the
            // engine convert. Rare enough to accept its conversion.
            float_format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            float_format.Format.nChannels = mix->nChannels;
            float_format.Format.nSamplesPerSec = mix->nSamplesPerSec;
            float_format.Format.wBitsPerSample = 32;
            float_format.Format.nBlockAlign = static_cast<WORD>(4u * mix->nChannels);
            float_format.Format.nAvgBytesPerSec = mix->nSamplesPerSec * float_format.Format.nBlockAlign;
            float_format.Format.cbSize = 22;
            float_format.Samples.wValidBitsPerSample = 32;
            float_format.dwChannelMask = 0;
            float_format.SubFormat = kSubtypeFloat;
            flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            use = &float_format.Format;
            describe_format(use, device_format_value);
        }
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 100000 /* 10 ms */, 0, use, nullptr);
        CoTaskMemFree(mix);
        mix = nullptr;
        if (FAILED(hr)) {
            set_error(hresult_text("IAudioClient::Initialize", hr));
            release(client);
            release(endpoint.device);
            return false;
        }
        REFERENCE_TIME default_period = 0;
        if (SUCCEEDED(client->GetDevicePeriod(&default_period, nullptr)) && default_period > 0) {
            period = (static_cast<AudioTimestamp64>(default_period) * device_format_value.rate + 5000000u) /
                     10000000u;
        }
        endpoint.client = client;
    }

    hr = endpoint.client->GetBufferSize(&endpoint.buffer_frames);
    if (SUCCEEDED(hr)) hr = endpoint.client->SetEventHandle(audio_event);
    if (SUCCEEDED(hr)) {
        hr = endpoint.client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&endpoint.render));
    }
    // Measurement only (docs/FRAME_TELEMETRY.md, AudioClock): the device's own clock against QPC, to measure how the
    // audio device, the display and the game clock drift apart. A failure only leaves the rows out.
    if (SUCCEEDED(hr) && shadow::render::present_trace::enabled() &&
        FAILED(endpoint.client->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(&endpoint.clock)))) {
        endpoint.clock = nullptr;
    }
    if (FAILED(hr)) {
        set_error(hresult_text("IAudioClient setup", hr));
        close_endpoint(endpoint);
        return false;
    }
    if (period == 0u) period = endpoint.buffer_frames;  // UINT32 buffer, widened here
    if (!configure_consumer(device_format_value, period)) {
        close_endpoint(endpoint);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(info_mutex);
        device_name = std::move(name);
        buffer_frames = endpoint.buffer_frames;  // UINT32 GetBufferSize, host count is 64-bit
        low_latency = low_latency_period;
    }
    consecutive_errors = 0;
    wakes_at_open = get(kWakes);
    device_open.store(true);
    // Stepped 0/1 next to the other device_open transition in close_endpoint:
    // it draws the exact windows in which no endpoint existed - the windows in
    // which discard_excess is throwing audio away and in which the producer's
    // overflow counter would otherwise be misread.
    GEKKO_PROFILE_PLOT("aud/endpoint-open", 1.0);
    // Without this a capture cannot be interpreted at all. Every reading in the
    // aud/ set is relative to numbers that today exist only in
    // AudioRuntime::Report, which goes to stdout and never enters the capture:
    // a 512 ms queue-depth plot is unreadable without knowing the ring is
    // 16,384 frames, and a 2.67 ms period cannot be told from a 10 ms one.
    // `name` was moved into device_name above, so the text is read back from
    // the members under the same lock report() uses.
    if (GEKKO_PROFILE_CONNECTED()) {
        char text[384];
        int length = 0;
        {
            std::lock_guard<std::mutex> lock(info_mutex);
            length = std::snprintf(
                text, sizeof(text),
                "aud/endpoint-config device=\"%s\" %s %u Hz %u ch period=%llu buffer=%llu %s "
                "mmcss=%d resampler=%llu/%llu taps=%u ring=%llu frames",
                device_name.c_str(), device_format.c_str(), device_rate, device_channels,
                static_cast<unsigned long long>(period_frames),
                static_cast<unsigned long long>(buffer_frames),
                low_latency ? "IAudioClient3" : "fallback-10ms", mmcss ? 1 : 0,
                static_cast<unsigned long long>(resampler_up),
                static_cast<unsigned long long>(resampler_down), resampler_taps,
                static_cast<unsigned long long>(ring_capacity));
        }
        if (length > 0) {
            GEKKO_PROFILE_MESSAGE(text, static_cast<std::size_t>(length) < sizeof(text)
                                            ? static_cast<std::size_t>(length)
                                            : sizeof(text) - 1u);
        }
    }
    return true;
}

void AudioRuntime::Impl::close_endpoint(Endpoint& endpoint) noexcept {
    if (endpoint.client != nullptr && endpoint.started) endpoint.client->Stop();
    release(endpoint.clock);
    release(endpoint.render);
    release(endpoint.client);
    release(endpoint.device);
    endpoint.id.clear();
    endpoint.buffer_frames = 0;
    endpoint.started = false;
    device_open.store(false);
    device_started.store(false);
    GEKKO_PROFILE_PLOT("aud/endpoint-open", 0.0);
}

bool AudioRuntime::Impl::endpoint_failed(Endpoint& endpoint, HRESULT hr, const char* what) {
    set_error(hresult_text(what, hr));
    // Device losses are end-of-run scalars today (kInvalidations, kRenderErrors,
    // kDefaultChanges, kReopens); on a timeline the question is WHEN the
    // endpoint died and what the audio queue was doing at that instant, because
    // an invalidation mid-stage and one at load are different bugs. Reads back
    // the text set_error just stored instead of formatting it again, and covers
    // both exits below - the HRESULT in the text says which one it was.
    if (GEKKO_PROFILE_CONNECTED()) {
        std::lock_guard<std::mutex> lock(info_mutex);
        GEKKO_PROFILE_MESSAGE(last_error.c_str(), last_error.size());
    }
    const bool gone = hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING
#ifdef AUDCLNT_E_RESOURCES_INVALIDATED
                      || hr == AUDCLNT_E_RESOURCES_INVALIDATED
#endif
        ;
    if (gone) {
        add(kInvalidations, 1u);
        close_endpoint(endpoint);
        return false;
    }
    add(kRenderErrors, 1u);
    // A client that keeps failing for another reason is rebuilt too.
    if (++consecutive_errors >= 50u) close_endpoint(endpoint);
    return false;
}

bool AudioRuntime::Impl::render_endpoint(Endpoint& endpoint) {
    if (config.fault_invalidate_after_wakes != 0u &&
        get(kWakes) - wakes_at_open >= config.fault_invalidate_after_wakes) {
        return endpoint_failed(endpoint, AUDCLNT_E_DEVICE_INVALIDATED,
                               "injected invalidation (Config::fault_invalidate_after_wakes)");
    }
    // Covers the WASAPI calls as well as the mix, which is the point:
    // kMaxRenderUs fuses "the endpoint blocked" with "our mixer was slow" and
    // cannot tell them apart. Scoped over the available_u32 == 0 early return
    // and every endpoint_failed exit as well. 100-750/s (IAudioClient3 minimum
    // period, or the 10 ms fallback).
    GEKKO_PROFILE_ZONE("aud/device-wake");
    const auto begin = std::chrono::steady_clock::now();
    UINT32 padding = 0;
    HRESULT hr = endpoint.client->GetCurrentPadding(&padding);
    if (FAILED(hr)) return endpoint_failed(endpoint, hr, "IAudioClient::GetCurrentPadding");
    // The device-side half of end-to-end latency, on the same axis as the
    // ring-side half (aud/queue-ms-*). This project has 630 ms of permanent
    // latency in its history that no single counter could attribute; with both
    // halves plotted, which side is holding the audio stops being a guess.
    GEKKO_PROFILE_PLOT("aud/device-pad-ms", 1000.0 * static_cast<double>(padding) /
                                                static_cast<double>(std::max<u32>(format.rate, 1u)));
    // WASAPI frame counts are UINT32. Widen before any host arithmetic so a
    // buffer-padding subtract cannot wrap a 32-bit host index.
    const UINT32 available_u32 = endpoint.buffer_frames - std::min(padding, endpoint.buffer_frames);
    if (audio_audit::enabled()) {
        audio_audit::event(audio_audit::Lane::Device, "endpoint-wake",
            padding, available_u32, producer.head.load(std::memory_order_acquire), tail,
            get(kOutput), get(kUnderrunFrames), endpoint.buffer_frames, format.rate);
    }
    GEKKO_PROFILE_VALUE(available_u32);
    if (available_u32 == 0u) return true;
    BYTE* data = nullptr;
    hr = endpoint.render->GetBuffer(available_u32, &data);
    if (FAILED(hr)) return endpoint_failed(endpoint, hr, "IAudioRenderClient::GetBuffer");
    const AudioFrameIndex64 frames = available_u32;
    const u64 missing_before = get(kUnderrunFrames);
    const u64 consumed_before = get(kConsumed);
    render(data, frames);
    hr = endpoint.render->ReleaseBuffer(available_u32, 0);
    if (audio_audit::enabled()) {
        // Device acceptance is separate from source starvation. This owner
        // lane is preallocated and flushed only after the threads retire.
        audio_audit::event(audio_audit::Lane::Device, "endpoint-submit",
            frames, get(kConsumed) - consumed_before, get(kUnderrunFrames) - missing_before,
            static_cast<u32>(hr), producer.head.load(std::memory_order_acquire), tail,
            get(kOutput), get(kPrimeSilence));
    }
    if (FAILED(hr)) return endpoint_failed(endpoint, hr, "IAudioRenderClient::ReleaseBuffer");
    consecutive_errors = 0;
    if (endpoint.clock != nullptr && begin - endpoint.clock_sampled >= std::chrono::seconds(1)) {
        endpoint.clock_sampled = begin;
        UINT64 position = 0, qpc_100ns = 0, frequency = 0;
        if (SUCCEEDED(endpoint.clock->GetFrequency(&frequency)) &&
            SUCCEEDED(endpoint.clock->GetPosition(&position, &qpc_100ns))) {
            shadow::render::present_trace::record(shadow::render::present_trace::Ev::AudioClock, position, frequency,
                                                  snapshot_fill(), qpc_100ns * 100u);
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    raise(kMaxRenderUs, static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
    return true;
}

bool AudioRuntime::Impl::start_endpoint(Endpoint& endpoint) {
    // IAudioClient::Start blocked 181-187 ms on a USB endpoint (Config comment
    // in audio_runtime.h); today that survives only as the kDeviceStartUs
    // scalar. As a zone it sits on the timeline where it happened and lines up
    // against the game thread's startup work. Also covers the priming
    // render_endpoint call below. 1/run; ~100/s in a failing-device loop.
    GEKKO_PROFILE_ZONE("aud/start-endpoint");
    // One buffer before Start (prime silence or real audio, whichever the
    // render pass decides), so the first period is not a glitch.
    if (!render_endpoint(endpoint)) return false;
    const auto begin = std::chrono::steady_clock::now();
    const HRESULT hr = endpoint.client->Start();
    const auto end = std::chrono::steady_clock::now();
    if (FAILED(hr)) {
        endpoint_failed(endpoint, hr, "IAudioClient::Start");
        if (endpoint.open()) close_endpoint(endpoint);
        return false;
    }
    counters[kDeviceStartUs].store(
        static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count()),
        std::memory_order_relaxed);
    started_at = end;
    awaiting_first_wake = true;
    endpoint.started = true;
    device_started.store(true);
    return true;
}

void AudioRuntime::Impl::consumer_main() {
    // First statement, so the thread carries a name even if COM init or the
    // enumerator fails. It is created unnamed in open(); every peer in the tree
    // is named (host-job-worker, flipper, present, game).
    GEKKO_PROFILE_THREAD("audio-wasapi");
    gekko::name_this_thread("audio-wasapi");  // ES-T1
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task_index = 0;
    const HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    {
        std::lock_guard<std::mutex> lock(info_mutex);
        mmcss = task != nullptr;
    }
    if (task == nullptr) {
        char text[96];
        std::snprintf(text, sizeof(text), "AvSetMmThreadCharacteristicsW(Pro Audio) failed (%lu)",
                      static_cast<unsigned long>(GetLastError()));
        set_error(text);  // not fatal: the thread still renders
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) set_error(hresult_text("CoCreateInstance(MMDeviceEnumerator)", hr));
    EndpointNotifier notifier(default_changed, control_event);
    const bool registered =
        enumerator != nullptr && SUCCEEDED(enumerator->RegisterEndpointNotificationCallback(&notifier));

    // start_device_at_open = false only: before Start there are no device
    // events, so the prime is polled at 1 ms. The start fill is the latency for
    // the whole run, and a coarse 15.6 ms wait would add up to that much.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == nullptr) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    bool timer_armed = false;

    // With no endpoint the ring is drained well inside its capacity (a quarter
    // of it, at most 100 ms) while opens are retried once a second, so the
    // producer's overflow counter keeps meaning a consumer stall and never a
    // missing device.
    const u64 ring_ms = producer.capacity * 1000u * kAudioSourceRateDen / kAudioSourceRateNum;
    const u64 drain_ms = std::clamp<u64>(ring_ms / 4u, 1u, 100u);
    constexpr auto kOpenRetry = std::chrono::milliseconds(1000);

    Endpoint endpoint;
    bool first = true;
    bool ever_opened = false;
    bool have_wake = false;
    auto last_wake = std::chrono::steady_clock::time_point{};
    auto next_open = std::chrono::steady_clock::time_point{};

    while (!stop.load()) {
        if (!endpoint.open()) {
            if (!first && default_changed.exchange(false)) {
                // A device appeared or the default moved: try it now.
                add(kDefaultChanges, 1u);
                next_open = std::chrono::steady_clock::time_point{};
            }
            if (std::chrono::steady_clock::now() >= next_open) {
                if (first && config.fault_first_open_delay_ms != 0u) {
                    WaitForSingleObject(control_event, config.fault_first_open_delay_ms);
                    if (stop.load()) break;
                }
                bool opened = enumerator != nullptr && open_endpoint(enumerator, endpoint);
                if (opened && config.start_device_at_open) {
                    // The endpoint runs from here on. Its start-up transient
                    // (IAudioClient::Start blocked 181-187 ms on a USB endpoint)
                    // is absorbed by prime silence, not by queued guest audio.
                    holding = true;
                    opened = start_endpoint(endpoint);
                    holding = false;
                }
                if (opened) {
                    // Audio queued while no endpoint could play it (a gap, a
                    // slow activation, the Start() above) is stale. Keep one
                    // prime's worth and count the rest instead of carrying it as
                    // permanent latency. When open() is still waiting the
                    // producer is inside open() and the ring is empty, so this
                    // only acts on reopens and on opens that outlived the timeout.
                    // A lead (gekko/schedule_offset.h) is part of the queue that belongs there.
                    discard_excess(prime_frames +
                                   gekko::schedule_offset::applied_lead_frames(kAudioSourceRateNum, kAudioSourceRateDen));
                    if (ever_opened) add(kReopens, 1u);
                    ever_opened = true;
                } else {
                    add(kOpenFailures, 1u);
                    // The acquisition-side counterpart to the endpoint-lost
                    // messages: a staircase whose slope is the retry rate.
                    // aud/endpoint-open says a device is absent; this says
                    // whether anything is trying to get one back.
                    GEKKO_PROFILE_PLOT("aud/open-failures", static_cast<double>(get(kOpenFailures)));
                    next_open = std::chrono::steady_clock::now() + kOpenRetry;
                }
                if (first) {
                    first = false;
                    // A failed first open ends the thread only if open() is
                    // still waiting to fall back to Null. Past its timeout the
                    // runtime is live in Wasapi mode and this thread must retry.
                    const bool open_waiting = finish_handshake(opened);
                    if (!opened && open_waiting) break;
                }
            }
            if (!endpoint.open()) {
                discard_excess(0u);
                const auto now = std::chrono::steady_clock::now();
                u64 wait_ms = drain_ms;
                if (next_open > now) {
                    const auto until = std::chrono::duration_cast<std::chrono::milliseconds>(next_open - now).count();
                    wait_ms = std::min<u64>(wait_ms, static_cast<u64>(until) + 1u);
                }
                if (timer != nullptr) {
                    // High resolution: a small ring needs a short, honest period.
                    LARGE_INTEGER due{};
                    due.QuadPart = -static_cast<LONGLONG>(wait_ms) * 10000;
                    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                    HANDLE handles[2] = {control_event, timer};
                    // Same name as the main-loop wait below. Instrumenting only
                    // that one would make the reading "time outside aud/wait and
                    // outside aud/device-wake is the scheduler taking the thread
                    // away" false for the whole no-endpoint window, where the
                    // thread sleeps here instead. ~10/s (drain_ms).
                    GEKKO_PROFILE_ZONE("aud/wait");
                    WaitForMultipleObjects(2u, handles, FALSE, static_cast<DWORD>(wait_ms) + 50u);
                    CancelWaitableTimer(timer);
                    timer_armed = false;
                } else {
                    GEKKO_PROFILE_ZONE("aud/wait");
                    WaitForSingleObject(control_event, static_cast<DWORD>(wait_ms));
                }
                continue;
            }
            have_wake = false;
        }

        bool fast_poll = false;
        if (!endpoint.started) {
            sync_epoch();
            const u64 fill = producer.head.load(std::memory_order_relaxed) - tail;
            // Audio arriving: poll at 1 ms so the start fill lands on the prime.
            // Nothing queued: a silent game must not cost 1 kHz wakeups.
            fast_poll = fill != 0u && fill < prime_frames;
            if (timer_armed && !fast_poll) {
                CancelWaitableTimer(timer);
                timer_armed = false;
            }
            if (fill >= prime_frames) {
                if (!start_endpoint(endpoint)) {
                    WaitForSingleObject(control_event, 10);  // a failing client must not spin
                    continue;
                }
            } else if (fast_poll && !timer_armed && timer != nullptr) {
                LARGE_INTEGER due{};
                due.QuadPart = -10000;  // 1 ms, relative
                timer_armed = SetWaitableTimer(timer, &due, 1, nullptr, nullptr, FALSE) != 0;
            }
        }

        HANDLE handles[3] = {control_event, audio_event, timer};
        const DWORD count = timer != nullptr ? 3u : 2u;
        const DWORD timeout = endpoint.started ? 2000u : timer_armed ? 100u : fast_poll ? 1u : 10u;
        // The zone has to close at the wait and not at the end of the iteration,
        // or it would swallow render_endpoint and "time outside aud/wait" would
        // mean nothing - that is the only reason for the block. This project has
        // already paid once for being unable to separate a descheduled thread
        // from a waiting one. 100-750/s started, up to 1 kHz during the 1 ms
        // fast_poll prime.
        DWORD wait;
        {
            GEKKO_PROFILE_ZONE("aud/wait");
            wait = WaitForMultipleObjects(count, handles, FALSE, timeout);
        }
        if (stop.load()) break;

        if (default_changed.exchange(false) && endpoint.open() && enumerator != nullptr &&
            default_endpoint_differs(enumerator, endpoint)) {
            add(kDefaultChanges, 1u);
            close_endpoint(endpoint);
            continue;
        }
        if (!endpoint.started) continue;

        if (wait == WAIT_OBJECT_0 + 1u) {
            const auto now = std::chrono::steady_clock::now();
            if (awaiting_first_wake) {
                awaiting_first_wake = false;
                counters[kFirstWakeUs].store(
                    static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(now - started_at).count()),
                    std::memory_order_relaxed);
            }
            if (have_wake) {
                const u64 interval = static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::microseconds>(now - last_wake).count());
                raise(kMaxWakeIntervalUs, interval);
                // kMaxWakeIntervalUs is one scalar per run and cannot say
                // whether it was one 200 ms event or a hundred 20 ms ones. The
                // existing late threshold (interval * 2 > period_us * 3) then
                // becomes a horizontal reading on this plot for free.
                GEKKO_PROFILE_PLOT("aud/wake-ms", static_cast<double>(interval) / 1000.0);
                const AudioTimestamp64 period_us =
                    period_frames * 1000000u / std::max<u32>(format.rate, 1u);
                if (interval * 2u > period_us * 3u) add(kLateWakes, 1u);
            }
            last_wake = now;
            have_wake = true;
            render_endpoint(endpoint);
            // A named frame gives Tracy's frame graph and frame-time histogram
            // for the audio callback; the distribution's tail IS the late
            // wakes, which aud/wake-ms shows as a time series and this shows as
            // a distribution. Its own namespace: the only other frame mark in
            // the tree is "Present".
            GEKKO_PROFILE_FRAME("aud/period");
        } else if (wait == WAIT_TIMEOUT) {
            // A started stream with no event for 2 s: ask the client why.
            add(kWakeTimeouts, 1u);
            UINT32 padding = 0;
            hr = endpoint.client->GetCurrentPadding(&padding);
            if (FAILED(hr)) endpoint_failed(endpoint, hr, "IAudioClient::GetCurrentPadding (timeout)");
        }
    }

    close_endpoint(endpoint);
    if (timer != nullptr) {
        CancelWaitableTimer(timer);
        CloseHandle(timer);
    }
    if (registered) enumerator->UnregisterEndpointNotificationCallback(&notifier);
    release(enumerator);
    if (task != nullptr) AvRevertMmThreadCharacteristics(task);
    if (SUCCEEDED(com)) CoUninitialize();
    if (first) finish_handshake(false);
}
#endif  // _WIN32

// ---------------------------------------------------------------------------
// AudioRuntime
// ---------------------------------------------------------------------------

AudioRuntime::AudioRuntime() : impl_(std::make_unique<Impl>()) {}
AudioRuntime::~AudioRuntime() { close(); }

AudioRuntime::Mode AudioRuntime::mode() const noexcept { return impl_->mode.load(); }

bool AudioRuntime::open(const Config& config) {
    close();
    Impl& impl = *impl_;
    impl.clear();
    impl.config = config;
    // Declared here and not after the ring allocation below, because Mode::Null
    // returns before that: in Null mode the producer still runs in counting mode
    // (publish_block moves head with ring == nullptr), so the producer plots
    // would otherwise fire unconfigured in exactly the headless runs this
    // project benchmarks with. step is load-bearing - a cumulative counter drawn
    // interpolated turns a 3.1 s production hole into a gentle ramp, and a
    // sampled queue depth drawn interpolated invents values between wakes.
    GEKKO_PROFILE_PLOT_CONFIG("aud/queue-ms-produce", GEKKO_PLOT_NUMBER, true, true, 0x4FC3F7);
    GEKKO_PROFILE_PLOT_CONFIG("aud/queue-ms-consume", GEKKO_PLOT_NUMBER, true, true, 0x81C784);
    GEKKO_PROFILE_PLOT_CONFIG("aud/device-pad-ms", GEKKO_PLOT_NUMBER, true, true, 0xFFB74D);
    GEKKO_PROFILE_PLOT_CONFIG("aud/wake-ms", GEKKO_PLOT_NUMBER, true, false, 0xBA68C8);
    GEKKO_PROFILE_PLOT_CONFIG("aud/frames-consumed", GEKKO_PLOT_NUMBER, true, false, 0x90A4AE);
    GEKKO_PROFILE_PLOT_CONFIG("aud/underrun-frames", GEKKO_PLOT_NUMBER, true, false, 0xE53935);
    GEKKO_PROFILE_PLOT_CONFIG("aud/drops", GEKKO_PLOT_NUMBER, true, false, 0xEF5350);
    GEKKO_PROFILE_PLOT_CONFIG("aud/drop-frames", GEKKO_PLOT_NUMBER, true, false, 0xC62828);
    GEKKO_PROFILE_PLOT_CONFIG("aud/gap-discarded", GEKKO_PLOT_NUMBER, true, false, 0xFF8A65);
    GEKKO_PROFILE_PLOT_CONFIG("aud/open-failures", GEKKO_PLOT_NUMBER, true, false, 0xD4E157);
    GEKKO_PROFILE_PLOT_CONFIG("aud/endpoint-open", GEKKO_PLOT_NUMBER, true, true, 0x64B5F6);
    GEKKO_PROFILE_PLOT_CONFIG("aud/resampler-bytes", GEKKO_PLOT_MEMORY, true, false, 0xA1887F);
    if (config.mode == Mode::Null) {
        impl.mode.store(Mode::Null);
        return true;
    }

    const u64 capacity = round_up_pow2(
        std::clamp<u64>(config.ring_frames, kMinRingFrames, kMaxRingFrames));
    try {
        impl.ring_storage.assign(static_cast<std::size_t>(capacity) * 2u, 0);
        impl.feed.assign(static_cast<std::size_t>(kFeedFrames) * 2u, 0);
        impl.scratch.assign(static_cast<std::size_t>(kScratchFrames) * 2u, 0.0f);
    } catch (const std::bad_alloc&) {
        impl.release_ring();
        impl.set_error("audio ring allocation failed");
        return false;
    }
    impl.producer.ring = impl.ring_storage.data();
    // A named pool, not the aggregate Domain::Audio byte count: that count has
    // no lifetime and no call stack, so it cannot say when the 512 ms ring
    // existed, whether a close()/open() pair leaked it, or that its size is a
    // policy choice (16,384 frames x 2 ch x 2 B = 64 KiB). Freed in
    // release_ring() from the same pointer.
    GEKKO_PROFILE_ALLOC(impl.producer.ring, static_cast<std::size_t>(capacity) * 2u * sizeof(s16),
                        "audio-ring");
    impl.producer.capacity = capacity;
    impl.producer.mask = capacity - 1u;
    {
        std::lock_guard<std::mutex> lock(impl.info_mutex);
        impl.ring_capacity = capacity;
    }

    if (config.mode == Mode::Manual) {
        if (!impl.configure_consumer(config.manual_format, config.manual_period_frames)) {
            impl.release_ring();
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(impl.info_mutex);
            impl.device_name = "manual";
        }
        impl.mode.store(Mode::Manual);
        return true;
    }

#if defined(_WIN32)
    impl.control_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    impl.audio_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (impl.control_event == nullptr || impl.audio_event == nullptr) {
        impl.set_error("CreateEventW failed");
        close();
        return false;
    }
    impl.stop.store(false);
    impl.default_changed.store(false);
    impl.open_result = -1;
    impl.open_abandoned = false;
    impl.fault_opens_failed = 0;
    impl.wakes_at_open = 0;
    impl.mode.store(Mode::Wasapi);
    try {
        impl.consumer_thread = std::thread([&impl] { impl.consumer_main(); });
    } catch (const std::system_error&) {
        impl.set_error("audio consumer thread creation failed");
        close();
        return false;
    }
    // ES-W7: the handshake's deadline is fixed here, whether the wait happens
    // now or in wait_open().
    impl.handshake_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(config.open_timeout_ms);
    if (config.defer_handshake) {
        impl.handshake_deferred = true;
        impl.open_pending_at_return.store(true);  // until wait_open() decides
        return true;
    }
    return finish_first_open();
#else
    impl.set_error("WASAPI output is Windows-only");
    impl.release_ring();
    return false;
#endif
}

// The first-open handshake, unchanged from what open() always did; ES-W7 only
// lets it run later (wait_open) against the deadline open() fixed.
bool AudioRuntime::finish_first_open() {
#if defined(_WIN32)
    Impl& impl = *impl_;
    int result = -1;
    {
        std::unique_lock<std::mutex> lock(impl.handshake_mutex);
        impl.handshake_cv.wait_until(lock, impl.handshake_deadline, [&impl] { return impl.open_result >= 0; });
        result = impl.open_result;
        // Decided under the lock the consumer reports through: from here a
        // failed first open belongs to the consumer, which keeps retrying.
        if (result < 0) impl.open_abandoned = true;
    }
    // Still opening past the timeout (result < 0): the runtime is live, the
    // consumer finishes or retries on its own, and trims what queues meanwhile
    // to one prime once an endpoint starts. Decided before the failure branch:
    // a deferred open() had set the flag while the handshake was pending.
    impl.open_pending_at_return.store(result < 0);
    if (result == 0) {
        // No endpoint: keep last_error, count only. The game carries on.
        close();
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool AudioRuntime::wait_open() {
    Impl& impl = *impl_;
    if (!impl.handshake_deferred) return impl.mode.load() != Mode::Null || impl.config.mode == Mode::Null;
    impl.handshake_deferred = false;
    return finish_first_open();
}

void AudioRuntime::close() {
    Impl& impl = *impl_;
#if defined(_WIN32)
    if (impl.consumer_thread.joinable()) {
        impl.stop.store(true);
        SetEvent(impl.control_event);
        impl.consumer_thread.join();
    }
    // AU-T1: the consumer has stopped, so its open gap (if any) is readable
    // here; it is recorded as open at close, and every record is printed.
    if (impl.record_open) impl.open_record.open_at_close = true;
    impl.commit_underrun_record();
    impl.print_underrun_records();
    if (impl.control_event != nullptr) {
        CloseHandle(impl.control_event);
        impl.control_event = nullptr;
    }
    if (impl.audio_event != nullptr) {
        CloseHandle(impl.audio_event);
        impl.audio_event = nullptr;
    }
#endif
    impl.release_ring();
    impl.mode.store(Mode::Null);
    impl.device_open.store(false);
    impl.device_started.store(false);
    impl.handshake_deferred = false;  // ES-W7: nothing left to wait for
}

void AudioRuntime::publish_block(HostAudioPointer64 bytes, AudioByteOffset64 size) noexcept {
    if (bytes == nullptr || size == 0u) return;
    // The production drumbeat on the game thread: NativeAudio calls this once
    // per finished AI pass (640 bytes), 200 times per game second. A guest
    // stall is a visible gap in this row, and whatever zone is open beside it
    // is the mechanism. The value makes a pass of another size visible.
    GEKKO_PROFILE_ZONE("aud/publish-block");
    GEKKO_PROFILE_VALUE(size);
    // AU-T1: print finished underrun records here, on the game thread (one
    // relaxed / acquire pair per block; the device thread never prints).
    if (impl_->underrun_records_written.load(std::memory_order_acquire) !=
        impl_->underrun_records_read.load(std::memory_order_relaxed)) {
        impl_->print_underrun_records();
    }
    Impl::Producer& p = impl_->producer;
    p.blocks.store(p.blocks.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    const AudioByteOffset64 carried = p.carry_bytes;
    const AudioByteOffset64 pending = carried + size;
    const AudioFrameIndex64 frames = pending / 4u;
    if (frames == 0u) {
        std::memcpy(p.carry + carried, bytes, size);
        p.carry_bytes = static_cast<u32>(pending);
        return;
    }
    // The old carry is completed by frame 0, so the new carry is the tail of
    // `bytes`. Keeping it even when the block is dropped keeps sample
    // alignment for everything after the drop.
    const AudioByteOffset64 next_carry = pending % 4u;
    const SampleCounter64 head = p.head.load(std::memory_order_relaxed);
    if (p.ring != nullptr) {
        const AudioFrameIndex64 fill = head - impl_->consumer.tail.load(std::memory_order_acquire);
        if (p.capacity - fill < frames) {
            p.dropped_frames.store(p.dropped_frames.load(std::memory_order_relaxed) + frames,
                                   std::memory_order_relaxed);
            p.dropped_events.store(p.dropped_events.load(std::memory_order_relaxed) + 1u,
                                   std::memory_order_relaxed);
            // Both counters, not just the event count: one 512 ms loss and one
            // 8-frame loss are different bugs and an event count cannot tell
            // them apart. Free in the healthy case - this branch is the only
            // site. The consumer drains the ring to a quarter of capacity while
            // no endpoint exists, which is what keeps these meaning a consumer
            // stall and never a missing device.
            GEKKO_PROFILE_PLOT("aud/drops",
                               static_cast<double>(p.dropped_events.load(std::memory_order_relaxed)));
            GEKKO_PROFILE_PLOT("aud/drop-frames",
                               static_cast<double>(p.dropped_frames.load(std::memory_order_relaxed)));
            std::memcpy(p.carry, bytes + static_cast<std::size_t>(size - next_carry),
                        static_cast<std::size_t>(next_carry));
            p.carry_bytes = static_cast<u32>(next_carry);
            return;
        }
        const auto write_run = [&p](SampleCounter64 position, HostAudioPointer64 source,
                                    AudioFrameIndex64 count) noexcept {
            const AudioFrameIndex64 index = position & p.mask;
            const AudioFrameIndex64 first = std::min(count, p.capacity - index);
            convert_be_frames(p.ring + static_cast<std::size_t>(index) * 2u, source, first);
            if (first < count) {
                convert_be_frames(p.ring, source + static_cast<std::size_t>(first) * 4u, count - first);
            }
        };
        SampleCounter64 position = head;
        AudioByteOffset64 offset = 0;
        if (carried != 0u) {
            u8 joined[4];
            std::memcpy(joined, p.carry, static_cast<std::size_t>(carried));
            std::memcpy(joined + static_cast<std::size_t>(carried), bytes,
                        static_cast<std::size_t>(4u - carried));
            write_run(position, joined, 1u);
            ++position;
            offset = 4u - carried;
        }
        write_run(position, bytes + static_cast<std::size_t>(offset), head + frames - position);
        const AudioFrameIndex64 new_fill = fill + frames;
        if (new_fill > p.max_fill.load(std::memory_order_relaxed)) {
            p.max_fill.store(new_fill, std::memory_order_relaxed);
        }
        // Inside this branch because new_fill only exists where a ring does;
        // counting mode (ring == nullptr) has no fill, and the drop path above
        // has already returned. Source ms, the same conversion Impl::render
        // uses for its fill histogram; the 16,384-frame ring spans 0-512 ms.
        GEKKO_PROFILE_PLOT("aud/queue-ms-produce",
                           1000.0 * static_cast<double>(new_fill) *
                               static_cast<double>(kAudioSourceRateDen) /
                               static_cast<double>(kAudioSourceRateNum));
    }
    std::memcpy(p.carry, bytes + static_cast<std::size_t>(size - next_carry),
                static_cast<std::size_t>(next_carry));
    p.carry_bytes = static_cast<u32>(next_carry);
    // PCM writes precede publication; the consumer acquires this prefix.
    p.head.store(head + frames, std::memory_order_release);
}

void AudioRuntime::publish_silence(AudioFrameIndex64 frames) noexcept {
    if (frames == 0u) return;
    // A distinct failure shape from a stall, so it must not be fused with
    // aud/publish-block or a silent run hides inside the block count: an AI
    // pass whose buffer is not host-backed, so the DAC consumed the time as
    // silence while the timeline kept moving. 0/s normally; it shares
    // publish_block's 200/s ceiling in the bad case.
    GEKKO_PROFILE_ZONE("aud/publish-silence");
    GEKKO_PROFILE_VALUE(frames);
    Impl::Producer& p = impl_->producer;
    if (p.carry_bytes != 0u) {
        // Outside the DMA contract (whole blocks only). The carried bytes stay
        // put, so alignment holds; the silence just lands one frame early.
        p.silence_split.store(p.silence_split.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    }
    p.silence.store(p.silence.load(std::memory_order_relaxed) + frames, std::memory_order_relaxed);
    const u64 head = p.head.load(std::memory_order_relaxed);
    if (p.ring != nullptr) {
        const u64 fill = head - impl_->consumer.tail.load(std::memory_order_acquire);
        if (p.capacity - fill < frames) {
            p.dropped_frames.store(p.dropped_frames.load(std::memory_order_relaxed) + frames,
                                   std::memory_order_relaxed);
            p.dropped_events.store(p.dropped_events.load(std::memory_order_relaxed) + 1u,
                                   std::memory_order_relaxed);
            // The same pair as in publish_block: the counters are shared, so
            // both overflow sites have to publish them or the staircase misses
            // steps on whichever path was not instrumented.
            GEKKO_PROFILE_PLOT("aud/drops",
                               static_cast<double>(p.dropped_events.load(std::memory_order_relaxed)));
            GEKKO_PROFILE_PLOT("aud/drop-frames",
                               static_cast<double>(p.dropped_frames.load(std::memory_order_relaxed)));
            return;
        }
        const u64 index = head & p.mask;
        const u64 first = std::min<u64>(frames, p.capacity - index);
        std::memset(p.ring + index * 2u, 0, static_cast<std::size_t>(first) * 2u * sizeof(s16));
        std::memset(p.ring, 0, static_cast<std::size_t>(frames - first) * 2u * sizeof(s16));
        const u64 new_fill = fill + frames;
        if (new_fill > p.max_fill.load(std::memory_order_relaxed)) {
            p.max_fill.store(new_fill, std::memory_order_relaxed);
        }
    }
    p.head.store(head + frames, std::memory_order_release);
}

void AudioRuntime::reset_timeline() noexcept {
    Impl::Producer& p = impl_->producer;
    if (p.carry_bytes != 0u) {
        p.reset_partial_bytes.store(p.reset_partial_bytes.load(std::memory_order_relaxed) + p.carry_bytes,
                                    std::memory_order_relaxed);
    }
    p.carry_bytes = 0;
    p.reset_position.store(p.head.load(std::memory_order_relaxed), std::memory_order_release);
    p.epoch.store(p.epoch.load(std::memory_order_relaxed) + 1u, std::memory_order_release);
}

void AudioRuntime::finish_production() noexcept {
    impl_->production_finished.store(true, std::memory_order_release);
    impl_->print_underrun_records();
}

void AudioRuntime::Impl::print_underrun_records() noexcept {
    u64 read = underrun_records_read.load(std::memory_order_relaxed);
    const u64 written = underrun_records_written.load(std::memory_order_acquire);
    while (read != written) {
        const UnderrunRecord r = underrun_records[static_cast<std::size_t>(read % kUnderrunRecords)];
        ++read;
        underrun_records_read.store(read, std::memory_order_release);
        const double rate = r.device_rate != 0u ? static_cast<double>(r.device_rate) : 1.0;
        const double source_ms = 1000.0 * static_cast<double>(kAudioSourceRateDen) /
                                 static_cast<double>(kAudioSourceRateNum);
        std::fprintf(stderr,
                     "[audio-underrun] gap %llu: %llu device frames (%.2f ms at %u Hz) of silence at audio time "
                     "%.3f s; queue before %.2f ms; after %.3f s of device output%s\n",
                     static_cast<unsigned long long>(read), static_cast<unsigned long long>(r.device_frames),
                     1000.0 * static_cast<double>(r.device_frames) / rate, r.device_rate,
                     static_cast<double>(r.source_frame) * source_ms / 1000.0,
                     static_cast<double>(r.queue_before_frames) * source_ms,
                     static_cast<double>(r.output_before) / rate,
                     r.open_at_close ? " (still open at close: no audio followed it)" : "");
    }
}

AudioFrameIndex64 AudioRuntime::consume_source_frames(s16* interleaved,
                                                      AudioFrameIndex64 max_frames) noexcept {
    Impl& impl = *impl_;
    if (impl.mode.load() != Mode::Manual || interleaved == nullptr) return 0u;
    impl.sync_epoch();
    return impl.read_ring(interleaved, max_frames);
}

void AudioRuntime::render(u8* out, AudioFrameIndex64 device_frames) noexcept {
    Impl& impl = *impl_;
    if (impl.mode.load() != Mode::Manual || out == nullptr || device_frames == 0u) return;
    impl.render(out, device_frames);
}

SampleCounter64 AudioRuntime::source_fill_frames() const noexcept { return impl_->snapshot_fill(); }

AudioRuntime::Report AudioRuntime::report() const {
    const Impl& impl = *impl_;
    Report r;
    r.mode = impl.mode.load();
    r.device_open = impl.device_open.load();
    r.device_started = impl.device_started.load();
    r.open_pending_at_return = impl.open_pending_at_return.load();
    r.source_rate_hz = static_cast<double>(kAudioSourceRateNum) / static_cast<double>(kAudioSourceRateDen);

    // Consumer side before producer side: the consumer never runs ahead of the
    // producer, so this order keeps consumed <= produced and fill from wrapping
    // in a snapshot taken while both threads run.
    using C = Impl::Counter;
    r.ring_fill_frames = impl.snapshot_fill();
    r.consumed_frames = impl.get(C::kConsumed);
    r.reset_dropped_frames = impl.get(C::kResetDropped);
    r.device_gap_discarded_frames = impl.get(C::kGapDiscarded);

    const u64 dropped = impl.producer.dropped_frames.load(std::memory_order_relaxed);
    const u64 head = impl.producer.head.load(std::memory_order_relaxed);
    r.blocks = impl.producer.blocks.load(std::memory_order_relaxed);
    r.produced_frames = head + dropped;
    r.silence_frames = impl.producer.silence.load(std::memory_order_relaxed);
    r.dropped_frames = dropped;
    r.dropped_events = impl.producer.dropped_events.load(std::memory_order_relaxed);
    r.reset_epochs = impl.producer.epoch.load(std::memory_order_relaxed);
    r.reset_partial_bytes = impl.producer.reset_partial_bytes.load(std::memory_order_relaxed);
    r.silence_split_samples = impl.producer.silence_split.load(std::memory_order_relaxed);
    r.ring_max_fill_frames = impl.producer.max_fill.load(std::memory_order_relaxed);

    r.epochs_honoured = impl.get(C::kEpochsHonoured);
    r.output_frames = impl.get(C::kOutput);
    r.underrun_events = impl.get(C::kUnderrunEvents);
    r.underrun_frames = impl.get(C::kUnderrunFrames);
    r.trailing_silence_frames = impl.get(C::kTrailingSilence);
    r.underrun_records_dropped = impl.get(C::kUnderrunRecordsDropped);
    r.prime_silence_frames = impl.get(C::kPrimeSilence);
    r.clipped_samples = impl.get(C::kClipped);
    r.prime_frames = impl.get(C::kPrimeFrames);
    r.start_fill_frames = impl.get(C::kStartFill);
    r.starts = impl.get(C::kStarts);
    r.device_start_us = impl.get(C::kDeviceStartUs);
    r.first_wake_us = impl.get(C::kFirstWakeUs);
    r.wakes = impl.get(C::kWakes);
    r.wake_timeouts = impl.get(C::kWakeTimeouts);
    r.late_wakes = impl.get(C::kLateWakes);
    r.max_wake_interval_us = impl.get(C::kMaxWakeIntervalUs);
    r.max_render_us = impl.get(C::kMaxRenderUs);
    r.reopens = impl.get(C::kReopens);
    r.open_failures = impl.get(C::kOpenFailures);
    r.device_invalidations = impl.get(C::kInvalidations);
    r.default_device_changes = impl.get(C::kDefaultChanges);
    r.render_errors = impl.get(C::kRenderErrors);
    for (std::size_t i = 0; i < Report::kFillBuckets; ++i) {
        r.fill_histogram[i] = impl.fill_histogram[i].load(std::memory_order_relaxed);
    }

    std::lock_guard<std::mutex> lock(impl.info_mutex);
    r.ring_capacity_frames = impl.ring_capacity;
    r.device_name = impl.device_name;
    r.device_format = impl.device_format;
    r.device_rate = impl.device_rate;
    r.device_channels = impl.device_channels;
    r.device_period_frames = impl.period_frames;
    r.device_buffer_frames = impl.buffer_frames;
    r.device_period_ms =
        impl.device_rate != 0u ? 1000.0 * impl.period_frames / static_cast<double>(impl.device_rate) : 0.0;
    r.low_latency_period = impl.low_latency;
    r.mmcss = impl.mmcss;
    r.resampler_up = impl.resampler_up;
    r.resampler_down = impl.resampler_down;
    r.resampler_taps = impl.resampler_taps;
    r.last_error = impl.last_error;
    return r;
}

}  // namespace gekko
