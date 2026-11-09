#pragma once

// Host audio runtime (docs/native-runtime-2026-09-16/ARCHITECTURE.md §6).
//
// The native audio platform (NativeAudio) publishes each finished AI pass - the
// guest's own stereo mix, 160 frames per 5 ms of game time - as it ends. This
// is the other end of that pipe. It replaces HostAudio + SDL:
//
//   game thread   publish_block / publish_silence / reset_timeline
//                 big-endian s16 -> native frames -> lock-free SPSC ring
//   own thread    WASAPI shared mode, event driven, MMCSS "Pro Audio"
//                 ring -> fixed-ratio polyphase resampler -> device mix format
//
// The rules this file exists to keep (memory: gekkoforge-pacer-catchup-ratchet,
// gekkoforge-parallelism-ceiling, understand/07 §6.2):
// * The playback ratio is fixed when an endpoint opens. There is no rate servo:
//   the ring level never feeds back into pitch, the pacer or the guest.
// * The ring is a stall bound, not a latency target. Overflow drops the
//   incoming block whole; an underrun plays silence and the device keeps
//   running. Neither happens without a counter.
// * The producer never locks, allocates or makes a system call. It pays a few
//   relaxed atomic loads/stores per call (plain moves on x64), never per sample.
//
// Windows headers stay in audio_runtime.cpp.

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "gekko/hw/types.h"
#include "gekko/types.h"

namespace gekko {

using hw::AudioByteOffset64;
using hw::AudioFrameIndex64;
using hw::AudioTimestamp64;
using hw::HostAudioPointer64;
using hw::SampleCounter64;

// The source rate is the game's DSP sample rate in game time: an AI pass of
// 160 frames lasts exactly 5 ms of game time (202,500 time-base ticks), so the
// stream carries exactly 32,000 frames per game second. (The instruction-clock
// runtime read one 32-byte block per 121,496 retired instructions, 32,001.0535
// frames per guest second; that artefact is gone with the instruction clock.)
// Kept as a rational so the resampler's ratio stays exact.
inline constexpr u64 kAudioSourceRateNum = 32000ull;
inline constexpr u64 kAudioSourceRateDen = 1ull;

enum class AudioSampleKind : u8 {
    Float32,
    Float64,
    Int16,
    Int24,  // 3-byte container
    Int32,  // 4-byte container; valid_bits says how many are significant
};

struct AudioDeviceFormat {
    u32 rate = 48000;
    u32 channels = 2;
    AudioSampleKind kind = AudioSampleKind::Float32;
    u32 valid_bits = 32;  // Int32 only: 24 or 32 (left-justified)

    u32 bytes_per_sample() const noexcept;
    u32 bytes_per_frame() const noexcept { return bytes_per_sample() * channels; }
};

// Fixed-ratio polyphase windowed-sinc resampler for interleaved stereo.
//
// The ratio is exact: output/input = output_rate * den / num, reduced to up/down.
// The instant of the next output frame is held as an integer input index plus a
// numerator in [0, up), so no floating-point position ever accumulates error.
// Coefficients come from a Kaiser-windowed sinc sampled at kPhases phases and
// interpolated linearly between adjacent phases.
//
// Output frame k sits at input instant k * down / up, with no added delay. It
// needs half_taps() frames of look-ahead, so after N pushed frames exactly
// ceil((N - half_taps) * up / down) frames can be pulled (N >= half_taps).
//
// Every output frame is computed by the same code over the same contiguous
// window whatever the push/pull chunking, so streaming is bit-identical to
// one-shot processing.
class AudioResampler {
public:
    static constexpr u32 kPhases = 256;
    // 80 taps at or above the source rate. Measured by audio_runtime_tests: the
    // realized response is flat within 0.04 dB to 14 kHz and attenuates at least
    // 86 dB from the source Nyquist to 96 kHz (analytic, 10 Hz grid).
    static constexpr u32 kBaseHalfTaps = 40;
    static constexpr u32 kHistoryFrames = 4096;  // input held at once beyond the taps

    AudioResampler() = default;

    // Input rate is source_num / source_den Hz. Allocates; not for a hot path.
    // Returns false for a zero or unrepresentable ratio.
    bool configure(u64 source_num, u64 source_den, u32 output_rate);
    // Drops held input and restarts the output clock at the next pushed frame.
    void reset() noexcept;

    bool configured() const noexcept { return up_ != 0; }
    u64 up() const noexcept { return up_; }
    u64 down() const noexcept { return down_; }
    u32 half_taps() const noexcept { return half_taps_; }
    u32 taps() const noexcept { return taps_; }
    double cutoff_hz() const noexcept { return cutoff_hz_; }

    // More input frames push() needs before pull() can return output_frames.
    AudioFrameIndex64 input_needed(AudioFrameIndex64 output_frames) const noexcept;
    // Input frames push() can accept now.
    AudioFrameIndex64 input_room() const noexcept;
    // Appends interleaved stereo frames; returns how many were accepted.
    AudioFrameIndex64 push(const s16* interleaved, AudioFrameIndex64 frames) noexcept;
    // Produces up to max_frames interleaved stereo float frames (s16 scale
    // 1/32768) from held input. A null out advances the clock identically
    // without computing samples.
    AudioFrameIndex64 pull(float* interleaved, AudioFrameIndex64 max_frames) noexcept;

    // Diagnostic: |H(f)| of the realized kernel (row-normalized phases with the
    // linear phase interpolation included), f in Hz at the input rate.
    double response(double hz) const;

private:
    u64 up_ = 0;
    u64 down_ = 0;
    u32 half_taps_ = 0;
    u32 taps_ = 0;
    double source_hz_ = 0.0;
    double cutoff_hz_ = 0.0;
    std::vector<float> base_;   // kPhases x taps, each row sums to 1
    std::vector<float> slope_;  // next row minus this row
    std::vector<float> left_;
    std::vector<float> right_;
    AudioFrameIndex64 capacity_ = 0;
    AudioFrameIndex64 write_ = 0;   // one past the newest held frame (host)
    AudioFrameIndex64 center_ = 0;  // held frame at or before the next output instant
    u64 phase_ = 0;                 // next output instant = center_ + phase_ / up_
};

class AudioRuntime {
public:
    enum class Mode : u8 {
        Null,    // counting only: no ring, no consumer (headless)
        Wasapi,  // ring + consumer thread on the default render endpoint
        Manual,  // ring, no thread: the caller drives the consumer (tests, capture)
    };

    struct Config {
        Mode mode = Mode::Wasapi;
        // Stall bound in stereo frames, rounded up to a power of two (min 64).
        // 16,384 frames is ~512 ms of guest audio. NOT a latency target.
        AudioFrameIndex64 ring_frames = 16384;
        // Play after the ring holds this many device periods plus this many
        // ms of source audio. The prime is the whole latency budget: it is not
        // re-established after an underrun.
        u32 prime_device_periods = 2;
        u32 prime_extra_ms = 20;
        // Wasapi: run the endpoint from open (open() waits for Start), rendering
        // counted prime silence until the prime is queued. Starting it only once
        // the prime is queued lets the endpoint's own start-up transient pile up
        // in the ring, and with no rate servo that pile is permanent latency.
        // Measured on a Focusrite USB endpoint, 40 ms prime: IAudioClient::Start
        // blocked 181-187 ms; steady fill 190-210 ms starting after the prime,
        // 30-60 ms starting at open. After every endpoint start the queue is
        // trimmed to one prime (counted), so an open that outlives
        // open_timeout_ms, or a reopen, does not keep its Start() pile either.
        // false keeps start-after-prime for A/B.
        bool start_device_at_open = true;
        // A savestate load breaks the timeline anyway; re-establish the prime
        // there (counted as prime_silence_frames) instead of starting the new
        // timeline with no cushion. The device keeps running either way.
        bool reprime_on_reset = true;
        // Linear fade at gap edges (underrun and prime silence), 0 = hard edges.
        u32 gap_fade_ms = 2;
        // Manual mode: the format render() writes and its period for the prime.
        AudioDeviceFormat manual_format{};
        AudioFrameIndex64 manual_period_frames = 480;
        // Wasapi: how long open() waits for the consumer's first endpoint open.
        u32 open_timeout_ms = 5000;
        // ES-W7 (AU): Wasapi only. open() starts the consumer and returns at once
        // (open_pending_at_return); wait_open() then waits for the same handshake,
        // against the same deadline (open_timeout_ms from open()), with the same
        // outcomes. The caller does other start-up work (the render device) in
        // between, and calls wait_open() before the producer publishes anything.
        bool defer_handshake = false;

        // Fault injection (Wasapi only; 0 = off, never set in the game). The
        // endpoint-loss paths cannot be reached on demand with real hardware,
        // so these make the consumer behave exactly as if the endpoint had
        // done it. audio_runtime_tests drives them.
        u32 fault_first_open_delay_ms = 0;     // stall before the first endpoint open (slow activation)
        u32 fault_failed_opens = 0;            // this many endpoint open attempts fail as if no device existed
        u32 fault_invalidate_after_wakes = 0;  // each time an endpoint has rendered this many wakes it reports
                                               // AUDCLNT_E_DEVICE_INVALIDATED
    };

    struct Report {
        static constexpr std::size_t kFillBuckets = 128;
        static constexpr u32 kFillBucketMs = 5;

        Mode mode = Mode::Null;
        bool device_open = false;
        bool device_started = false;
        // open() returned at open_timeout_ms with the first endpoint open still
        // running; device_open says how that went.
        bool open_pending_at_return = false;
        double source_rate_hz = 0.0;

        // Producer (game thread).
        u64 blocks = 0;            // publish_block calls with data
        u64 produced_frames = 0;   // accepted + dropped, silence included
        u64 silence_frames = 0;    // publish_silence frames
        u64 dropped_frames = 0;    // ring full: incoming block dropped whole
        u64 dropped_events = 0;
        u64 reset_epochs = 0;      // reset_timeline calls
        u64 reset_partial_bytes = 0;    // bytes of a carried partial sample a reset dropped
        u64 silence_split_samples = 0;  // publish_silence calls inside a carried partial sample

        // Ring.
        u64 ring_capacity_frames = 0;
        u64 ring_fill_frames = 0;
        u64 ring_max_fill_frames = 0;  // observed by the producer after each accept

        // Consumer.
        u64 consumed_frames = 0;              // read from the ring into the resampler
        u64 reset_dropped_frames = 0;         // queued frames discarded by an epoch
        u64 epochs_honoured = 0;
        u64 device_gap_discarded_frames = 0;  // no endpoint, or excess over the prime after an endpoint start
        u64 output_frames = 0;                // device frames rendered, silence included
        // Audible gaps: each audio -> silence transition after the start. A
        // wake that plays audio and then runs short opens a new event; a wake
        // with no audio at all continues the previous one.
        // Silence after finish_production() is not counted here (AU-T1). Each
        // event is also one "[audio-underrun] gap N" line on stderr, printed by
        // the producer's thread once audio resumes (or at finish / close).
        u64 underrun_events = 0;
        u64 underrun_frames = 0;              // device frames of silence after the start
        u64 trailing_silence_frames = 0;      // AU-T1: device frames of silence after finish_production()
        u64 underrun_records_dropped = 0;     // AU-T1: gap lines lost to a full record ring (64)
        u64 prime_silence_frames = 0;         // device frames of silence while priming (open, reset, reopen)
        u64 clipped_samples = 0;
        u64 prime_frames = 0;                 // source frames
        u64 start_fill_frames = 0;            // ring fill when playback last (re)started
        u64 starts = 0;                       // playback (re)starts after a prime
        u64 device_start_us = 0;              // last IAudioClient::Start call
        u64 first_wake_us = 0;                // last Start to its first device event
        u64 wakes = 0;                        // consumer render passes
        u64 wake_timeouts = 0;
        u64 late_wakes = 0;                   // event interval > 1.5 device periods
        u64 max_wake_interval_us = 0;
        u64 max_render_us = 0;
        u64 reopens = 0;
        u64 open_failures = 0;
        u64 device_invalidations = 0;
        u64 default_device_changes = 0;
        u64 render_errors = 0;
        // Ring fill at each consumer wake, in 5 ms buckets of source time; the
        // last bucket holds everything above.
        std::array<u64, kFillBuckets> fill_histogram{};

        // Endpoint.
        std::string device_name;
        std::string device_format;
        u32 device_rate = 0;
        u32 device_channels = 0;
        AudioFrameIndex64 device_period_frames = 0;
        AudioFrameIndex64 device_buffer_frames = 0;
        double device_period_ms = 0.0;
        bool low_latency_period = false;  // IAudioClient3 minimum period in use
        bool mmcss = false;

        u64 resampler_up = 0;
        u64 resampler_down = 0;
        u32 resampler_taps = 0;

        std::string last_error;
    };

    AudioRuntime();
    ~AudioRuntime();

    AudioRuntime(const AudioRuntime&) = delete;
    AudioRuntime& operator=(const AudioRuntime&) = delete;

    // Resets every counter. Returns true when the requested mode is live and
    // false only when the runtime fell back to Null (the producer keeps
    // counting; last_error says why). Losing sound must never stop the game.
    //
    // Wasapi starts the consumer and waits up to open_timeout_ms for its first
    // endpoint open:
    // * opened in time: true, the endpoint is running (start_device_at_open);
    // * failed in time: false, Null;
    // * still opening at the timeout: true, Wasapi, open_pending_at_return.
    //   The consumer finishes (or keeps retrying) on its own, exactly as after
    //   a lost device, and trims whatever queued meanwhile to one prime.
    // From then on report().device_open, not this result, says whether an
    // endpoint is playing. Call from the producer thread while it is not
    // publishing.
    bool open(const Config& config);
    // ES-W7: after open() with Config::defer_handshake, waits for the first
    // endpoint open (until open_timeout_ms after open()) and returns what open()
    // would have returned without the deferral: false (and Null) when the first
    // open failed in time, true otherwise (open_pending_at_return if still
    // opening). Without a deferred handshake it returns at once: true unless the
    // runtime is Null after a failed open. Call before the producer publishes.
    bool wait_open();
    // Joins the consumer thread and frees the ring. Counters stay readable.
    void close();
    Mode mode() const noexcept;

    // ---- producer: the game thread only ------------------------------------
    // Exactly the bytes the AI DMA engine read: big-endian s16 stereo, normally
    // 32. Any size is accepted; a sample split across calls is carried exactly.
    void publish_block(HostAudioPointer64 big_endian_s16_stereo, AudioByteOffset64 bytes) noexcept;
    // Frames of silence for a block the engine read without a host pointer,
    // so the timeline stays exact. Call it on a sample boundary (the DMA engine
    // always is: it moves whole 32-byte blocks). Inside a carried partial
    // sample the silence is queued before that sample, one frame early, and
    // silence_split_samples counts it; byte alignment is unaffected.
    void publish_silence(AudioFrameIndex64 frames) noexcept;
    // Savestate load: queued audio belongs to the old timeline. The consumer
    // discards it at its next wake (counted), and a carried partial sample is
    // dropped (reset_partial_bytes).
    void reset_timeline() noexcept;
    // AU-T1: the game will publish nothing more (the run is ending). Silence the
    // device plays from here on is the end of the run, not a gap: it counts as
    // trailing_silence_frames, never as an underrun. Prints the pending gap lines.
    // The audio itself is unchanged.
    void finish_production() noexcept;

    // ---- Manual mode consumer: one thread, no other consumer ---------------
    // Raw source frames in order, honouring resets. Bypasses the resampler.
    AudioFrameIndex64 consume_source_frames(s16* interleaved, AudioFrameIndex64 max_frames) noexcept;
    // The device render pass exactly as the WASAPI thread runs it, into
    // manual_format. Always writes device_frames frames.
    void render(u8* out, AudioFrameIndex64 device_frames) noexcept;

    // Frames currently queued (either thread; a snapshot).
    SampleCounter64 source_fill_frames() const noexcept;

    // Any thread. The producer never takes the lock this may take.
    Report report() const;

private:
    bool finish_first_open();  // the first-open handshake (open(), or wait_open() when deferred)
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gekko
