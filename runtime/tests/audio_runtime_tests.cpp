// AudioRuntime tests: ring, producer accounting, consumer render path and the
// polyphase resampler. No audio device is needed; `--device` plays a 440 Hz
// tone through WASAPI for 2 s and prints the counters (manual check).

#include "gekko/audio_runtime.h"
#include "gekko/hw/types.h"
#include "gekko/schedule_offset.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

using gekko::AudioDeviceFormat;
using gekko::AudioResampler;
using gekko::AudioRuntime;
using gekko::AudioSampleKind;
using gekko::s16;
using gekko::s32;
using gekko::s64;
using gekko::u16;
using gekko::u32;
using gekko::u64;
using gekko::u8;

using Clock = std::chrono::steady_clock;

constexpr double kPi = 3.14159265358979323846;
const double kSourceHz =
    static_cast<double>(gekko::kAudioSourceRateNum) / static_cast<double>(gekko::kAudioSourceRateDen);
// The source rate is exact in game time (an AI pass of 160 frames is 5 ms), so
// a second of source audio is a whole number of frames.
static_assert(gekko::kAudioSourceRateNum % gekko::kAudioSourceRateDen == 0u,
              "the source rate is a whole number of frames per second");
constexpr u64 kSourceFramesPerSecond = gekko::kAudioSourceRateNum / gekko::kAudioSourceRateDen;

int g_check_failures = 0;

void check(bool condition, int line, const char* text) {
    if (!condition) {
        ++g_check_failures;
        std::printf("    CHECK failed (line %d): %s\n", line, text);
    }
}
#define CHECK(condition) check(static_cast<bool>(condition), __LINE__, #condition)

// splitmix64: deterministic on every compiler and standard library.
struct Rng {
    u64 state;
    explicit Rng(u64 seed) : state(seed) {}
    u64 next() {
        state += 0x9E3779B97F4A7C15ull;
        u64 z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    u64 range(u64 low, u64 high) { return low + next() % (high - low + 1u); }
};

// A frame value that identifies its index uniquely below 2^32.
s16 left_of(u64 index) { return static_cast<s16>(static_cast<u16>(index)); }
s16 right_of(u64 index) {
    return static_cast<s16>(static_cast<u16>((index >> 16) ^ 0x5a5au ^ (index * 7u)));
}

void encode(u8* out, s16 left, s16 right) {
    out[0] = static_cast<u8>(static_cast<u16>(left) >> 8);
    out[1] = static_cast<u8>(left);
    out[2] = static_cast<u8>(static_cast<u16>(right) >> 8);
    out[3] = static_cast<u8>(right);
}

void encode_index(u8* out, u64 index) { encode(out, left_of(index), right_of(index)); }

std::vector<u8> encode_range(u64 first, u64 count) {
    std::vector<u8> bytes(static_cast<std::size_t>(count) * 4u);
    for (u64 i = 0; i < count; ++i) encode_index(bytes.data() + i * 4u, first + i);
    return bytes;
}

AudioRuntime::Config manual_config(u32 ring_frames, AudioDeviceFormat format = {}, u32 period = 480) {
    AudioRuntime::Config config;
    config.mode = AudioRuntime::Mode::Manual;
    config.ring_frames = ring_frames;
    config.manual_format = format;
    config.manual_period_frames = period;
    return config;
}

u64 expected_output_frames(u64 input_frames, const AudioResampler& resampler) {
    const u64 half = resampler.half_taps();
    if (input_frames < half) return 0;
    const u64 numerator = (input_frames - half) * resampler.up();
    return (numerator + resampler.down() - 1u) / resampler.down();
}

// ---------------------------------------------------------------------------
// Ring and producer
// ---------------------------------------------------------------------------

// One producer thread, one consumer thread, and a third thread reading report()
// throughout. `mixed` adds odd sizes that split samples across calls, and large
// blocks, to the real 32-byte DMA block.
void run_spsc_stress(u32 ring_frames, bool mixed, u64 seed) {
    AudioRuntime runtime;
    CHECK(runtime.open(manual_config(ring_frames)));
    const u64 capacity = runtime.report().ring_capacity_frames;
    CHECK(capacity == ring_frames);
    const u64 total = 10000000u;
    std::atomic<bool> done{false};
    u64 calls = 0;
    u64 full_waits = 0;
    const auto begin = Clock::now();

    std::atomic<bool> stop_reader{false};
    u64 reports = 0;
    u64 inconsistent_reports = 0;
    std::thread reader([&] {
        while (!stop_reader.load()) {
            const AudioRuntime::Report r = runtime.report();
            if (r.consumed_frames > r.produced_frames || r.ring_fill_frames > capacity ||
                runtime.source_fill_frames() > capacity) {
                ++inconsistent_reports;
            }
            ++reports;
            std::this_thread::yield();
        }
    });

    std::thread producer([&] {
        Rng rng(seed);
        u64 next = 0;
        while (next < total) {
            const u64 frames = std::min<u64>(4096u, total - next);
            const std::vector<u8> bytes = encode_range(next, frames);
            std::size_t offset = 0;
            while (offset < bytes.size()) {
                std::size_t size = 32u;
                if (mixed) {
                    const u64 pick = rng.range(0, 9);
                    size = pick < 7u ? 32u
                           : pick < 9u ? static_cast<std::size_t>(rng.range(1, 100))
                                       : static_cast<std::size_t>(rng.range(100, 2000));
                }
                size = std::min({size, bytes.size() - offset, static_cast<std::size_t>((capacity - 1u) * 4u)});
                // Never full: this case proves order and zero loss, not overflow.
                while (runtime.source_fill_frames() + size / 4u + 1u > capacity) {
                    ++full_waits;
                    std::this_thread::yield();
                }
                runtime.publish_block(bytes.data() + offset, size);
                ++calls;
                offset += size;
            }
            next += frames;
        }
        done.store(true);
    });

    std::vector<s16> buffer(4096u * 2u);
    Rng rng(seed ^ 0x5678u);
    u64 expected = 0;
    u64 mismatches = 0;
    for (;;) {
        const std::size_t want = static_cast<std::size_t>(rng.range(1, std::min<u64>(4096u, capacity + 7u)));
        const std::size_t got = runtime.consume_source_frames(buffer.data(), want);
        for (std::size_t i = 0; i < got; ++i) {
            if (buffer[i * 2u] != left_of(expected) || buffer[i * 2u + 1u] != right_of(expected)) ++mismatches;
            ++expected;
        }
        if (got == 0u) {
            if (done.load() && runtime.source_fill_frames() == 0u) break;
            std::this_thread::yield();
        }
    }
    producer.join();
    stop_reader.store(true);
    reader.join();
    const double seconds = std::chrono::duration<double>(Clock::now() - begin).count();
    const AudioRuntime::Report r = runtime.report();
    std::printf("    ring=%llu %s: frames=%llu calls=%llu mismatches=%llu dropped=%llu max_fill=%llu full_waits=%llu "
                "reports=%llu (inconsistent %llu) %.2f s\n",
                static_cast<unsigned long long>(capacity), mixed ? "mixed sizes" : "32-byte blocks",
                static_cast<unsigned long long>(expected), static_cast<unsigned long long>(calls),
                static_cast<unsigned long long>(mismatches), static_cast<unsigned long long>(r.dropped_frames),
                static_cast<unsigned long long>(r.ring_max_fill_frames), static_cast<unsigned long long>(full_waits),
                static_cast<unsigned long long>(reports), static_cast<unsigned long long>(inconsistent_reports),
                seconds);
    CHECK(inconsistent_reports == 0u);
    CHECK(expected == total);
    CHECK(mismatches == 0u);
    CHECK(r.produced_frames == total);
    CHECK(r.consumed_frames == total);
    CHECK(r.dropped_frames == 0u && r.dropped_events == 0u);
    CHECK(r.blocks == calls);
    CHECK(r.ring_max_fill_frames <= capacity && r.ring_max_fill_frames > 0u);
}

bool test_ring_spsc_stress() {
    run_spsc_stress(16384u, false, 0x1234u);
    run_spsc_stress(16384u, true, 0x2345u);
    run_spsc_stress(256u, true, 0x3456u);  // constant wrap-around and full-ring waits
    return true;
}

bool test_overflow_accounting() {
    AudioRuntime runtime;
    CHECK(runtime.open(manual_config(64)));
    CHECK(runtime.report().ring_capacity_frames == 64u);
    for (u64 block = 0; block < 9u; ++block) {
        const std::vector<u8> bytes = encode_range(block * 8u, 8u);
        runtime.publish_block(bytes.data(), bytes.size());
    }
    AudioRuntime::Report r = runtime.report();
    CHECK(r.produced_frames == 72u);
    CHECK(r.dropped_frames == 8u && r.dropped_events == 1u);
    CHECK(r.ring_fill_frames == 64u && r.ring_max_fill_frames == 64u);

    std::vector<s16> frames(128u * 2u);
    CHECK(runtime.consume_source_frames(frames.data(), 10u) == 10u);
    for (u64 i = 0; i < 10u; ++i) CHECK(frames[i * 2u] == left_of(i) && frames[i * 2u + 1u] == right_of(i));

    const std::vector<u8> ninth = encode_range(72u, 8u);  // fits: 54 + 8 = 62
    runtime.publish_block(ninth.data(), ninth.size());
    const std::vector<u8> tenth = encode_range(80u, 8u);  // 62 + 8 > 64: dropped whole
    runtime.publish_block(tenth.data(), tenth.size());
    runtime.publish_silence(2u);  // exactly fills
    runtime.publish_silence(1u);  // dropped
    r = runtime.report();
    CHECK(r.dropped_frames == 17u && r.dropped_events == 3u);
    CHECK(r.silence_frames == 3u);
    CHECK(r.produced_frames == 64u + 8u + 2u + 17u);
    CHECK(r.ring_fill_frames == 64u);

    const std::size_t got = runtime.consume_source_frames(frames.data(), 128u);
    CHECK(got == 64u);
    std::size_t k = 0;
    for (u64 i = 10; i < 64u; ++i, ++k) CHECK(frames[k * 2u] == left_of(i) && frames[k * 2u + 1u] == right_of(i));
    for (u64 i = 72; i < 80u; ++i, ++k) CHECK(frames[k * 2u] == left_of(i) && frames[k * 2u + 1u] == right_of(i));
    for (int i = 0; i < 2; ++i, ++k) CHECK(frames[k * 2u] == 0 && frames[k * 2u + 1u] == 0);
    std::printf("    produced=%llu dropped_frames=%llu dropped_events=%llu silence=%llu max_fill=%llu\n",
                static_cast<unsigned long long>(r.produced_frames), static_cast<unsigned long long>(r.dropped_frames),
                static_cast<unsigned long long>(r.dropped_events), static_cast<unsigned long long>(r.silence_frames),
                static_cast<unsigned long long>(r.ring_max_fill_frames));
    return true;
}

bool test_partial_sample_carry() {
    {
        AudioRuntime runtime;
        CHECK(runtime.open(manual_config(4096)));
        const std::vector<u8> stream = encode_range(0u, 1001u);
        const std::size_t length = 1000u * 4u + 3u;  // frame 1000 incomplete
        Rng rng(99);
        std::size_t offset = 0;
        while (offset < length) {
            const std::size_t size = std::min<std::size_t>(static_cast<std::size_t>(rng.range(1, 13)), length - offset);
            runtime.publish_block(stream.data() + offset, size);
            offset += size;
        }
        std::vector<s16> frames(2048u * 2u);
        CHECK(runtime.consume_source_frames(frames.data(), 2048u) == 1000u);
        for (u64 i = 0; i < 1000u; ++i) CHECK(frames[i * 2u] == left_of(i) && frames[i * 2u + 1u] == right_of(i));
        CHECK(runtime.report().produced_frames == 1000u);
        runtime.publish_block(stream.data() + length, 1u);
        CHECK(runtime.report().produced_frames == 1001u);
        CHECK(runtime.consume_source_frames(frames.data(), 2048u) == 1u);
        CHECK(frames[0] == left_of(1000u) && frames[1] == right_of(1000u));
    }
    {
        // A dropped block must not shift the byte alignment of what follows.
        AudioRuntime runtime;
        CHECK(runtime.open(manual_config(64)));
        const std::vector<u8> stream = encode_range(0u, 100u);
        runtime.publish_block(stream.data(), 2u);          // carry 2
        runtime.publish_block(stream.data() + 2u, 62u);    // frames 0..15
        runtime.publish_block(stream.data() + 64u, 190u);  // frames 16..62, carry 2
        CHECK(runtime.source_fill_frames() == 63u);
        runtime.publish_block(stream.data() + 254u, 8u);   // frames 63, 64: no room, dropped; carry 2
        std::vector<s16> frames(64u * 2u);
        CHECK(runtime.consume_source_frames(frames.data(), 64u) == 63u);
        for (u64 i = 0; i < 63u; ++i) CHECK(frames[i * 2u] == left_of(i) && frames[i * 2u + 1u] == right_of(i));
        runtime.publish_block(stream.data() + 262u, 2u);   // completes frame 65
        CHECK(runtime.consume_source_frames(frames.data(), 64u) == 1u);
        CHECK(frames[0] == left_of(65u) && frames[1] == right_of(65u));
        const AudioRuntime::Report r = runtime.report();
        CHECK(r.dropped_frames == 2u && r.dropped_events == 1u);
        CHECK(r.produced_frames == 66u);
    }
    {
        // Contract edges are counted, not silent. publish_silence inside a
        // carried partial sample (the DMA engine never does this) queues the
        // silence one frame early and keeps byte alignment; a reset drops the
        // carried bytes.
        AudioRuntime runtime;
        CHECK(runtime.open(manual_config(256)));
        u8 a[4];
        u8 b[4];
        encode(a, 0x1111, 0x2222);
        encode(b, 0x3333, 0x4444);
        runtime.publish_block(a, 2u);
        runtime.publish_silence(1u);
        runtime.publish_block(a + 2u, 2u);
        runtime.publish_block(b, 4u);
        s16 frames[8] = {};
        CHECK(runtime.consume_source_frames(frames, 4u) == 3u);
        CHECK(frames[0] == 0 && frames[1] == 0);
        CHECK(frames[2] == 0x1111 && frames[3] == 0x2222);
        CHECK(frames[4] == 0x3333 && frames[5] == 0x4444);
        runtime.publish_silence(1u);  // on a boundary: not counted
        runtime.publish_block(b, 3u);
        runtime.reset_timeline();
        AudioRuntime::Report r = runtime.report();
        CHECK(r.silence_split_samples == 1u && r.reset_partial_bytes == 3u);
        runtime.publish_block(a, 4u);
        CHECK(runtime.consume_source_frames(frames, 4u) == 1u);
        CHECK(frames[0] == 0x1111 && frames[1] == 0x2222);
        r = runtime.report();
        std::printf("    silence_split_samples=%llu reset_partial_bytes=%llu\n",
                    static_cast<unsigned long long>(r.silence_split_samples),
                    static_cast<unsigned long long>(r.reset_partial_bytes));
    }
    return true;
}

// Rapid producer resets overlap actual wrap-around reads. Coalescing may skip
// samples, but it must never invent/reorder PCM, reclaim an in-flight read, or
// let an observed reset boundary outrun the published head.
bool test_concurrent_epoch_reset() {
    AudioRuntime runtime;
    CHECK(runtime.open(manual_config(256u)));
    constexpr u32 kFrames = 200000u;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        u8 encoded[32];
        for (u32 first = 0; first < kFrames; first += 8u) {
            while (runtime.source_fill_frames() + 8u > 256u) std::this_thread::yield();
            for (u32 frame = 0; frame < 8u; ++frame) {
                const u32 id = first + frame;
                encode(encoded + frame * 4u, static_cast<s16>(id & 0xffffu),
                       static_cast<s16>(id >> 16u));
            }
            runtime.publish_block(encoded, sizeof(encoded));
            if ((first / 8u) % 3u == 0u) runtime.reset_timeline();
            if ((first / 8u) % 19u == 0u) std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });
    s16 samples[128];
    u64 read = 0, invalid = 0;
    u32 previous = 0;
    Rng rng(0x98127234u);
    for (;;) {
        const u64 got = runtime.consume_source_frames(samples, rng.range(1u, 64u));
        for (u64 frame = 0; frame < got; ++frame) {
            const u32 id = static_cast<u16>(samples[frame * 2u]) |
                           (static_cast<u32>(static_cast<u16>(samples[frame * 2u + 1u])) << 16u);
            if (id >= kFrames || (read != 0u && id <= previous)) ++invalid;
            previous = id;
            ++read;
        }
        if (got == 0u) {
            if (done.load(std::memory_order_acquire) && runtime.source_fill_frames() == 0u) break;
            std::this_thread::yield();
        }
    }
    producer.join();
    const auto r = runtime.report();
    std::printf("    concurrent reset: read=%llu skipped=%llu resets=%llu honoured=%llu invalid=%llu\n",
                read, r.reset_dropped_frames, r.reset_epochs, r.epochs_honoured, invalid);
    CHECK(read != 0u && invalid == 0u);
    CHECK(r.produced_frames == kFrames && r.consumed_frames == read);
    CHECK(r.consumed_frames + r.reset_dropped_frames == r.produced_frames);
    CHECK(r.dropped_frames == 0u && r.ring_fill_frames == 0u);
    CHECK(r.ring_max_fill_frames <= 256u);
    CHECK(r.epochs_honoured != 0u && r.epochs_honoured <= r.reset_epochs);
    return true;
}

bool test_epoch_reset() {
    AudioRuntime runtime;
    CHECK(runtime.open(manual_config(4096)));
    std::vector<s16> frames(4096u * 2u);

    const std::vector<u8> old_audio = encode_range(0u, 100u);
    runtime.publish_block(old_audio.data(), old_audio.size());
    runtime.publish_block(old_audio.data(), 2u);  // a dangling half sample
    runtime.reset_timeline();
    const std::vector<u8> new_audio = encode_range(5000u, 40u);
    runtime.publish_block(new_audio.data(), new_audio.size());
    CHECK(runtime.consume_source_frames(frames.data(), 4096u) == 40u);
    for (u64 i = 0; i < 40u; ++i) {
        CHECK(frames[i * 2u] == left_of(5000u + i) && frames[i * 2u + 1u] == right_of(5000u + i));
    }
    AudioRuntime::Report r = runtime.report();
    CHECK(r.reset_epochs == 1u && r.epochs_honoured == 1u);
    CHECK(r.reset_dropped_frames == 100u);

    // Two resets before the consumer looks: only the newest timeline survives.
    const std::vector<u8> a = encode_range(100u, 10u);
    const std::vector<u8> b = encode_range(200u, 5u);
    const std::vector<u8> c = encode_range(300u, 7u);
    runtime.publish_block(a.data(), a.size());
    runtime.reset_timeline();
    runtime.publish_block(b.data(), b.size());
    runtime.reset_timeline();
    runtime.publish_block(c.data(), c.size());
    CHECK(runtime.consume_source_frames(frames.data(), 4096u) == 7u);
    for (u64 i = 0; i < 7u; ++i) CHECK(frames[i * 2u] == left_of(300u + i));
    r = runtime.report();
    CHECK(r.reset_epochs == 3u && r.reset_dropped_frames == 115u);

    // A reset after a partial read discards only what was still queued.
    const std::vector<u8> d = encode_range(400u, 20u);
    runtime.publish_block(d.data(), d.size());
    CHECK(runtime.consume_source_frames(frames.data(), 5u) == 5u);
    runtime.reset_timeline();
    const std::vector<u8> e = encode_range(500u, 3u);
    runtime.publish_block(e.data(), e.size());
    CHECK(runtime.consume_source_frames(frames.data(), 4096u) == 3u);
    CHECK(frames[0] == left_of(500u));
    r = runtime.report();
    CHECK(r.reset_epochs == 4u && r.reset_dropped_frames == 130u);

    // Through the render path: the reset re-primes (counted as prime silence,
    // not an underrun) and the device never stops.
    AudioRuntime renderer;
    CHECK(renderer.open(manual_config(16384)));
    std::vector<u8> dc(4000u * 4u);
    for (u64 i = 0; i < 4000u; ++i) encode(dc.data() + i * 4u, 8192, 8192);
    std::vector<u8> out(480u * 8u);
    renderer.publish_block(dc.data(), dc.size());
    renderer.render(out.data(), 480u);
    renderer.reset_timeline();
    renderer.render(out.data(), 480u);
    r = renderer.report();
    CHECK(r.starts == 1u && r.prime_silence_frames == 480u && r.underrun_events == 0u);
    CHECK(r.reset_dropped_frames > 0u);
    renderer.publish_block(dc.data(), dc.size());
    renderer.render(out.data(), 480u);
    r = renderer.report();
    CHECK(r.starts == 2u && r.underrun_events == 0u && r.output_frames == 1440u);
    std::printf("    reset_epochs=%llu reset_dropped=%llu render: starts=%llu prime_silence=%llu underruns=%llu\n",
                static_cast<unsigned long long>(runtime.report().reset_epochs),
                static_cast<unsigned long long>(runtime.report().reset_dropped_frames),
                static_cast<unsigned long long>(r.starts), static_cast<unsigned long long>(r.prime_silence_frames),
                static_cast<unsigned long long>(r.underrun_events));
    return true;
}

bool test_null_mode() {
    AudioRuntime runtime;
    AudioRuntime::Config config;
    config.mode = AudioRuntime::Mode::Null;
    CHECK(runtime.open(config));
    CHECK(runtime.mode() == AudioRuntime::Mode::Null);
    const std::vector<u8> block = encode_range(0u, 8u);
    const auto begin = Clock::now();
    const u64 calls = 10000000u;
    for (u64 i = 0; i < calls; ++i) runtime.publish_block(block.data(), block.size());
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - begin).count() / calls;
    runtime.publish_block(block.data(), 3u);
    runtime.publish_block(block.data() + 3u, 1u);
    runtime.publish_silence(16u);
    runtime.reset_timeline();
    std::vector<s16> frames(64u);
    CHECK(runtime.consume_source_frames(frames.data(), 32u) == 0u);
    const AudioRuntime::Report r = runtime.report();
    CHECK(r.produced_frames == calls * 8u + 1u + 16u);
    CHECK(r.silence_frames == 16u && r.reset_epochs == 1u);
    CHECK(r.ring_capacity_frames == 0u && r.dropped_frames == 0u && r.consumed_frames == 0u);
    std::printf("    null publish_block(32 B): %.2f ns/call\n", ns);

    // The ring producer for comparison (no consumer; a ring large enough).
    AudioRuntime ring;
    CHECK(ring.open(manual_config(1u << 22)));
    const u64 ring_calls = (u64(1) << 22) / 8u;
    const auto ring_begin = Clock::now();
    for (u64 i = 0; i < ring_calls; ++i) ring.publish_block(block.data(), block.size());
    const double ring_ns = std::chrono::duration<double, std::nano>(Clock::now() - ring_begin).count() / ring_calls;
    CHECK(ring.report().dropped_frames == 0u);
    std::printf("    ring publish_block(32 B): %.2f ns/call over %llu calls\n", ring_ns,
                static_cast<unsigned long long>(ring_calls));
    return true;
}

// ---------------------------------------------------------------------------
// Consumer render path
// ---------------------------------------------------------------------------

float sample_f32(const std::vector<u8>& bytes, std::size_t index) {
    float value;
    std::memcpy(&value, bytes.data() + index * 4u, 4u);
    return value;
}

bool test_prime_underrun_resume() {
    AudioRuntime runtime;
    CHECK(runtime.open(manual_config(16384)));
    // 2 device periods of 480 frames at 48 kHz + 20 ms, in source frames, rounded up.
    const u64 expected_prime =
        (2u * 480u * gekko::kAudioSourceRateNum + gekko::kAudioSourceRateDen * 48000u - 1u) /
            (gekko::kAudioSourceRateDen * 48000u) +
        (20u * gekko::kAudioSourceRateNum + gekko::kAudioSourceRateDen * 1000u - 1u) /
            (gekko::kAudioSourceRateDen * 1000u);
    AudioRuntime::Report r = runtime.report();
    CHECK(r.prime_frames == expected_prime);
    const u64 prime = r.prime_frames;

    std::vector<u8> out(480u * 8u);
    std::vector<u8> dc(8192u * 4u);
    for (u64 i = 0; i < 8192u; ++i) encode(dc.data() + i * 4u, 8192, 8192);  // 0.25

    runtime.render(out.data(), 480u);
    r = runtime.report();
    CHECK(r.prime_silence_frames == 480u && r.starts == 0u && r.underrun_events == 0u);
    CHECK(std::all_of(out.begin(), out.end(), [](u8 v) { return v == 0; }));

    runtime.publish_block(dc.data(), (prime - 1u) * 4u);
    runtime.render(out.data(), 480u);
    CHECK(runtime.report().starts == 0u);
    runtime.publish_block(dc.data(), 4u);
    runtime.render(out.data(), 480u);
    r = runtime.report();
    CHECK(r.starts == 1u && r.underrun_events == 0u && r.start_fill_frames == prime);

    AudioResampler reference;
    reference.configure(gekko::kAudioSourceRateNum, gekko::kAudioSourceRateDen, 48000u);
    const u64 playable = expected_output_frames(prime, reference);
    std::size_t renders = 1;
    while (runtime.report().underrun_events == 0u && renders < 20u) {
        runtime.render(out.data(), 480u);
        ++renders;
    }
    r = runtime.report();
    CHECK(r.underrun_events == 1u);
    CHECK(r.output_frames - r.prime_silence_frames - r.underrun_frames == playable);
    const std::size_t played_in_last = static_cast<std::size_t>(480u - (r.underrun_frames));
    // The gap edge ramps the last value down instead of stepping to zero.
    const float before_gap = sample_f32(out, (played_in_last - 1u) * 2u);
    const float gap_first = sample_f32(out, played_in_last * 2u);
    CHECK(std::fabs(before_gap - 0.25f) < 1e-4f);
    CHECK(std::fabs(gap_first - 0.25f * 96.0f / 97.0f) < 1e-4f);
    const std::size_t decayed = 480u - played_in_last;

    runtime.render(out.data(), 480u);  // still starved: same event, more frames
    r = runtime.report();
    CHECK(r.underrun_events == 1u && r.underrun_frames == decayed + 480u);
    if (decayed < 96u) {
        // The 96-frame ramp continues across the wake, then true silence.
        CHECK(std::fabs(sample_f32(out, 0u) - 0.25f * static_cast<float>(96u - decayed) / 97.0f) < 1e-4f);
    }
    CHECK(sample_f32(out, 479u * 2u) == 0.0f);

    runtime.publish_block(dc.data(), dc.size());  // resume without reprime
    runtime.render(out.data(), 480u);
    r = runtime.report();
    CHECK(r.underrun_events == 1u && r.starts == 1u);
    // The first resumed frame fades in; the 2 ms ramp is 96 frames.
    CHECK(std::fabs(sample_f32(out, 0u) - 0.25f / 97.0f) < 1e-4f);
    CHECK(std::fabs(sample_f32(out, 200u * 2u) - 0.25f) < 1e-4f);

    while (runtime.report().underrun_events == 1u && renders < 100u) {
        runtime.render(out.data(), 480u);
        ++renders;
    }
    r = runtime.report();
    CHECK(r.underrun_events == 2u);
    CHECK(r.output_frames - r.prime_silence_frames - r.underrun_frames ==
          expected_output_frames(prime + 8192u, reference));
    std::printf("    prime=%llu playable=%llu underrun_events=%llu underrun_frames=%llu prime_silence=%llu output=%llu\n",
                static_cast<unsigned long long>(prime), static_cast<unsigned long long>(playable),
                static_cast<unsigned long long>(r.underrun_events), static_cast<unsigned long long>(r.underrun_frames),
                static_cast<unsigned long long>(r.prime_silence_frames), static_cast<unsigned long long>(r.output_frames));
    return true;
}

// gekko/schedule_offset.h: a lead the game runs ahead with is queued audio that leaving the lead takes back. A prime
// (first start, or the re-prime after a timeline reset) during a lead must therefore wait for the prime PLUS the lead,
// so that the queue left after the lead is given back is a full prime - the 2026-11-05 starvation (the boot primed one
// frame fuller, leaving the lead took the frame out, the queue fell to ~120 frames and ran dry).
bool test_prime_under_lead() {
    struct LeadReset {
        ~LeadReset() { gekko::schedule_offset::applied_lead_ns().store(0); }
    } restore;
    AudioRuntime runtime;
    CHECK(runtime.open(manual_config(16384)));
    const u64 prime = runtime.report().prime_frames;
    const std::int64_t lead_ns = 10'000'000;  // 10 ms
    const u64 lead_frames = static_cast<u64>(lead_ns) * gekko::kAudioSourceRateNum /
                            (gekko::kAudioSourceRateDen * 1'000'000'000ull);
    CHECK(lead_frames == 320u);
    std::vector<u8> out(480u * 8u);
    std::vector<u8> dc(16384u * 4u);
    for (u64 i = 0; i < 16384u; ++i) encode(dc.data() + i * 4u, 8192, 8192);

    // 1. First prime during a lead: a prime's worth is not enough, prime + lead is.
    gekko::schedule_offset::applied_lead_ns().store(lead_ns);
    runtime.publish_block(dc.data(), prime * 4u);
    runtime.render(out.data(), 480u);
    CHECK(runtime.report().starts == 0u);
    runtime.publish_block(dc.data(), lead_frames * 4u);
    runtime.render(out.data(), 480u);
    AudioRuntime::Report r = runtime.report();
    CHECK(r.starts == 1u && r.start_fill_frames == prime + lead_frames && r.underrun_events == 0u);

    // 2. A timeline reset during the lead re-primes against the same neutral threshold.
    runtime.reset_timeline();
    runtime.publish_block(dc.data(), prime * 4u);
    runtime.render(out.data(), 480u);
    r = runtime.report();
    const u64 starts_before = r.starts;
    runtime.publish_block(dc.data(), lead_frames * 4u);
    runtime.render(out.data(), 480u);
    r = runtime.report();
    CHECK(r.starts == starts_before + 1u && r.start_fill_frames == prime + lead_frames);

    // 3. No lead: the prime is exactly as before (test_prime_underrun_resume covers the details).
    gekko::schedule_offset::applied_lead_ns().store(0);
    runtime.reset_timeline();
    runtime.publish_block(dc.data(), (prime - 1u) * 4u);
    runtime.render(out.data(), 480u);
    r = runtime.report();
    const u64 starts_neutral = r.starts;
    runtime.publish_block(dc.data(), 4u);
    runtime.render(out.data(), 480u);
    r = runtime.report();
    CHECK(r.starts == starts_neutral + 1u && r.start_fill_frames == prime);
    std::printf("    prime=%llu lead=%llu start_fill(lead)=%llu starts=%llu\n", static_cast<unsigned long long>(prime),
                static_cast<unsigned long long>(lead_frames), static_cast<unsigned long long>(prime + lead_frames),
                static_cast<unsigned long long>(r.starts));
    return true;
}

// A producer steadily below real time (the guest has run at 76.8 % before):
// every wake plays some audio and then runs short. Each shortfall is its own
// audible gap and must be its own event. Found by the adversarial verifier: the
// old latch cleared only on a full wake and counted 1 event for 190 gaps.
//
// With gap_fade_ms = 0 the expected count comes from the output alone: a wake
// is [audio][silence], and an event starts wherever audio turns into silence,
// inside a wake or across a wake boundary. The fade only scales samples, so the
// 2 ms fade run must count identically.
bool test_underrun_event_per_gap() {
    u64 events_by_fade[2] = {};
    u64 frames_by_fade[2] = {};
    for (u32 fade_index = 0; fade_index < 2u; ++fade_index) {
        AudioRuntime::Config config = manual_config(16384);
        config.gap_fade_ms = fade_index == 0u ? 0u : 2u;
        AudioRuntime runtime;
        CHECK(runtime.open(config));
        std::vector<u8> dc(4096u * 4u);
        for (u64 i = 0; i < 4096u; ++i) encode(dc.data() + i * 4u, 8192, 8192);  // 0.25
        runtime.publish_block(dc.data(), 1400u * 4u);  // past the 1280-frame prime
        std::vector<u8> out(480u * 8u);
        u64 expected = 0;
        u64 gapped_wakes = 0;
        u64 silent_wakes = 0;
        u64 full_wakes = 0;
        u64 wrong_wakes = 0;
        bool previous_ended_in_audio = false;
        for (u32 wake = 0; wake < 240u; ++wake) {
            const u64 before = runtime.report().underrun_events;
            runtime.render(out.data(), 480u);
            const u64 delta = runtime.report().underrun_events - before;
            if (fade_index == 0u) {
                std::size_t audio = 0;
                while (audio < 480u && sample_f32(out, audio * 2u) != 0.0f) ++audio;
                bool shape_ok = true;  // everything after the audio prefix is silence
                for (std::size_t i = audio; i < 480u; ++i) shape_ok &= sample_f32(out, i * 2u) == 0.0f;
                CHECK(shape_ok);
                const bool starts_gap = audio < 480u && (audio != 0u || previous_ended_in_audio);
                if (audio == 480u) {
                    ++full_wakes;
                } else if (audio == 0u) {
                    ++silent_wakes;
                } else {
                    ++gapped_wakes;
                }
                if (delta != (starts_gap ? 1u : 0u)) ++wrong_wakes;
                expected += starts_gap ? 1u : 0u;
                previous_ended_in_audio = audio == 480u;
            }
            // 62.5 % of real time for 200 wakes, a 4-wake stall, then 2x to
            // recover into full wakes.
            if (wake < 200u) {
                runtime.publish_block(dc.data(), 200u * 4u);
            } else if (wake >= 204u) {
                runtime.publish_block(dc.data(), 640u * 4u);
            }
        }
        const AudioRuntime::Report r = runtime.report();
        events_by_fade[fade_index] = r.underrun_events;
        frames_by_fade[fade_index] = r.underrun_frames;
        if (fade_index == 0u) {
            std::printf("    fade 0: wakes full %llu, audio then gap %llu, silent %llu; events %llu expected %llu, "
                        "wakes with a wrong delta %llu, underrun_frames %llu\n",
                        static_cast<unsigned long long>(full_wakes), static_cast<unsigned long long>(gapped_wakes),
                        static_cast<unsigned long long>(silent_wakes), static_cast<unsigned long long>(r.underrun_events),
                        static_cast<unsigned long long>(expected), static_cast<unsigned long long>(wrong_wakes),
                        static_cast<unsigned long long>(r.underrun_frames));
            CHECK(gapped_wakes >= 190u && silent_wakes >= 3u && full_wakes >= 10u);
            CHECK(wrong_wakes == 0u);
            CHECK(r.underrun_events == expected);
        } else {
            std::printf("    fade 2 ms: events %llu, underrun_frames %llu\n",
                        static_cast<unsigned long long>(r.underrun_events),
                        static_cast<unsigned long long>(r.underrun_frames));
        }
    }
    CHECK(events_by_fade[0] == events_by_fade[1]);
    CHECK(frames_by_fade[0] == frames_by_fade[1]);
    return true;
}

// AU-T1: silence after finish_production() is the end of the run, not a gap.
// Two runtimes get the same blocks and the same renders; one is told the
// producer finished before its last starved wakes. The device output is
// identical byte for byte; only the accounting differs: the gap during play
// counts the same in both, the tail is trailing_silence_frames in the finished
// one and more underrun frames (no new event) in the other.
bool test_finish_production_tail() {
    std::vector<u8> outputs[2];
    AudioRuntime::Report reports[2];
    for (u32 finished = 0; finished < 2u; ++finished) {
        AudioRuntime runtime;
        CHECK(runtime.open(manual_config(16384)));
        std::vector<u8> dc(4096u * 4u);
        for (u64 i = 0; i < 4096u; ++i) encode(dc.data() + i * 4u, 8192, 8192);  // 0.25
        std::vector<u8> out(480u * 8u);
        std::vector<u8>& all = outputs[finished];
        const auto wake = [&] {
            runtime.render(out.data(), 480u);
            all.insert(all.end(), out.begin(), out.end());
        };
        runtime.publish_block(dc.data(), 1400u * 4u);  // past the 1280-frame prime
        for (u32 i = 0; i < 6u; ++i) wake();             // plays, then starves: gap 1
        const AudioRuntime::Report gap = runtime.report();
        CHECK(gap.underrun_events == 1u && gap.underrun_frames != 0u);
        // Audio resumes (gap 1 ends): 400 source frames are 600 device frames at
        // 48 kHz, so the 2nd wake plays some and starves again (gap 2, during play).
        runtime.publish_block(dc.data(), 400u * 4u);
        wake();
        wake();
        CHECK(runtime.report().underrun_events == 2u);
        if (finished != 0u) runtime.finish_production();
        for (u32 i = 0; i < 4u; ++i) wake();
        reports[finished] = runtime.report();
    }
    CHECK(outputs[0] == outputs[1]);  // the audio is unchanged
    const AudioRuntime::Report& open = reports[0];
    const AudioRuntime::Report& done = reports[1];
    CHECK(open.trailing_silence_frames == 0u);
    CHECK(done.trailing_silence_frames == 4u * 480u);
    CHECK(done.underrun_frames + done.trailing_silence_frames == open.underrun_frames);
    CHECK(open.underrun_events == done.underrun_events);
    CHECK(done.underrun_records_dropped == 0u && open.underrun_records_dropped == 0u);
    std::printf("    not finished: events %llu, underrun %llu, trailing %llu; finished: events %llu, underrun %llu, "
                "trailing %llu\n",
                static_cast<unsigned long long>(open.underrun_events),
                static_cast<unsigned long long>(open.underrun_frames),
                static_cast<unsigned long long>(open.trailing_silence_frames),
                static_cast<unsigned long long>(done.underrun_events),
                static_cast<unsigned long long>(done.underrun_frames),
                static_cast<unsigned long long>(done.trailing_silence_frames));
    return true;
}

bool test_device_formats() {
    struct Case {
        const char* name;
        AudioSampleKind kind;
        u32 channels;
        u32 valid_bits;
    };
    const Case cases[] = {
        {"float32 x2", AudioSampleKind::Float32, 2u, 32u}, {"float32 x6", AudioSampleKind::Float32, 6u, 32u},
        {"float64 x1", AudioSampleKind::Float64, 1u, 64u}, {"pcm16 x1", AudioSampleKind::Int16, 1u, 16u},
        {"pcm16 x2", AudioSampleKind::Int16, 2u, 16u},     {"pcm24 x4", AudioSampleKind::Int24, 4u, 24u},
        {"pcm32/24 x2", AudioSampleKind::Int32, 2u, 24u},  {"pcm32 x8", AudioSampleKind::Int32, 8u, 32u},
    };
    const double left = 0.25;
    const double right = -0.5;
    for (const Case& c : cases) {
        AudioDeviceFormat format;
        format.rate = 44100u;
        format.channels = c.channels;
        format.kind = c.kind;
        format.valid_bits = c.valid_bits;
        AudioRuntime runtime;
        CHECK(runtime.open(manual_config(16384, format, 441)));
        std::vector<u8> dc(6000u * 4u);
        for (u64 i = 0; i < 6000u; ++i) encode(dc.data() + i * 4u, 8192, -16384);
        runtime.publish_block(dc.data(), dc.size());
        const u32 frame_bytes = format.bytes_per_frame();
        std::vector<u8> out(441u * frame_bytes);
        for (int pass = 0; pass < 4; ++pass) runtime.render(out.data(), 441u);
        CHECK(runtime.report().underrun_events == 0u);
        // Read channel `ch` of the last frame as a normalised double.
        const auto read = [&](u32 ch) {
            const u8* p = out.data() + 440u * frame_bytes + ch * format.bytes_per_sample();
            switch (c.kind) {
                case AudioSampleKind::Float32: {
                    float v;
                    std::memcpy(&v, p, 4u);
                    return static_cast<double>(v);
                }
                case AudioSampleKind::Float64: {
                    double v;
                    std::memcpy(&v, p, 8u);
                    return v;
                }
                case AudioSampleKind::Int16: {
                    s16 v;
                    std::memcpy(&v, p, 2u);
                    return v / 32768.0;
                }
                case AudioSampleKind::Int24: {
                    s32 v = static_cast<s32>(p[0] | (p[1] << 8) | (p[2] << 16));
                    if (v & 0x800000) v -= 0x1000000;
                    return v / 8388608.0;
                }
                case AudioSampleKind::Int32: {
                    s32 v;
                    std::memcpy(&v, p, 4u);
                    if (c.valid_bits == 24u) CHECK((v & 0xff) == 0);
                    return v / 2147483648.0;
                }
            }
            return 0.0;
        };
        const double tolerance = c.kind == AudioSampleKind::Int16 ? 1.5 / 32768.0 : 2e-6;
        if (c.channels == 1u) {
            CHECK(std::fabs(read(0) - 0.5 * (left + right)) < tolerance);
        } else {
            CHECK(std::fabs(read(0) - left) < tolerance);
            CHECK(std::fabs(read(1) - right) < tolerance);
            for (u32 ch = 2; ch < c.channels; ++ch) CHECK(read(ch) == 0.0);
        }
        std::printf("    %-12s ch0=%+.6f%s\n", c.name, read(0), c.channels > 1u ? "" : " (L+R)/2");
    }

    // Full-scale square waves overshoot through the band limit: clipping is counted.
    AudioDeviceFormat pcm16;
    pcm16.kind = AudioSampleKind::Int16;
    AudioRuntime runtime;
    CHECK(runtime.open(manual_config(16384, pcm16, 480)));
    std::vector<u8> square(8000u * 4u);
    for (u64 i = 0; i < 8000u; ++i) {
        const s16 v = (i / 16u) % 2u == 0u ? s16(32767) : s16(-32768);
        encode(square.data() + i * 4u, v, v);
    }
    runtime.publish_block(square.data(), square.size());
    std::vector<u8> out(480u * 4u);
    for (int pass = 0; pass < 8; ++pass) runtime.render(out.data(), 480u);
    const u64 clipped = runtime.report().clipped_samples;
    CHECK(clipped > 0u);
    std::printf("    square wave clipped_samples=%llu\n", static_cast<unsigned long long>(clipped));
    return true;
}

bool test_render_chunking_bit_identity() {
    AudioDeviceFormat format;
    format.rate = 44100u;
    Rng noise(7);
    std::vector<u8> input(50000u * 4u);
    for (u8& byte : input) byte = static_cast<u8>(noise.next());
    const u64 total_frames = 60000u;
    const auto run = [&](bool random_chunks) {
        AudioRuntime runtime;
        CHECK(runtime.open(manual_config(65536, format, 441)));
        runtime.publish_block(input.data(), input.size());
        std::vector<u8> out(static_cast<std::size_t>(total_frames) * 8u);
        Rng rng(11);
        u64 done = 0;
        while (done < total_frames) {
            const u64 chunk = std::min<u64>(random_chunks ? rng.range(1, 2000) : 441u, total_frames - done);
            runtime.render(out.data() + done * 8u, static_cast<std::size_t>(chunk));
            done += chunk;
        }
        CHECK(runtime.report().underrun_events == 0u);
        return out;
    };
    const std::vector<u8> fixed = run(false);
    const std::vector<u8> random = run(true);
    CHECK(fixed == random);
    std::printf("    %llu device frames, fixed 441-frame wakes vs random 1..2000: %s\n",
                static_cast<unsigned long long>(total_frames), fixed == random ? "identical" : "DIFFERENT");

    // Consumer cost: 60 s of guest audio rendered to float32 stereo 48 kHz in
    // 10 ms wakes (resampler + conversion + ring reads).
    AudioRuntime runtime;
    CHECK(runtime.open(manual_config(1u << 21, AudioDeviceFormat{}, 480)));
    const u64 source_frames = 60u * kSourceFramesPerSecond;
    for (u64 i = 0; i < source_frames; i += 4096u) {
        const u64 count = std::min<u64>(4096u, source_frames - i);
        std::vector<u8> bytes(static_cast<std::size_t>(count) * 4u);
        for (u8& byte : bytes) byte = static_cast<u8>(noise.next());
        runtime.publish_block(bytes.data(), bytes.size());
    }
    std::vector<u8> out(480u * 8u);
    const u64 wakes = 59u * 100u;
    const auto begin = Clock::now();
    for (u64 i = 0; i < wakes; ++i) runtime.render(out.data(), 480u);
    const double seconds = std::chrono::duration<double>(Clock::now() - begin).count();
    CHECK(runtime.report().underrun_events == 0u);
    std::printf("    consumer cost: %.3f ms per second of 48 kHz output (%.1f us per 10 ms wake)\n",
                seconds * 1000.0 / (wakes / 100.0), seconds * 1e6 / wakes);
    return true;
}

bool test_fill_histogram() {
    // 5 ms buckets of source time. At exactly 32 kHz a bucket edge is an exact
    // frame count: 1600 frames is 50.000 ms, the first fill of bucket 10, and
    // one frame less is the last fill of bucket 9.
    static_assert(kSourceFramesPerSecond == 32000u, "the bucket edges below are 32 kHz frame counts");
    std::vector<u8> out(480u * 8u);
    {
        AudioRuntime below;
        CHECK(below.open(manual_config(16384)));
        std::vector<u8> audio(1599u * 4u, 0);
        below.publish_block(audio.data(), audio.size());  // 49.969 ms: bucket 9
        below.render(out.data(), 480u);
        CHECK(below.report().fill_histogram[9] == 1u);
    }
    AudioRuntime runtime;
    CHECK(runtime.open(manual_config(16384)));
    std::vector<u8> audio(1600u * 4u, 0);
    runtime.publish_block(audio.data(), audio.size());  // 50.000 ms: bucket 10
    runtime.render(out.data(), 480u);
    AudioRuntime::Report r = runtime.report();
    CHECK(r.fill_histogram[10] == 1u);
    for (int i = 0; i < 5; ++i) runtime.render(out.data(), 480u);
    r = runtime.report();
    u64 sum = 0;
    for (u64 bucket : r.fill_histogram) sum += bucket;
    CHECK(sum == r.wakes && r.wakes == 6u);
    return true;
}

// ---------------------------------------------------------------------------
// Resampler
// ---------------------------------------------------------------------------

std::vector<float> resample_all(AudioResampler& resampler, const std::vector<s16>& input) {
    std::vector<float> output;
    std::vector<float> chunk(8192u * 2u);
    std::size_t offset = 0;
    const std::size_t frames = input.size() / 2u;
    for (;;) {
        std::size_t produced = resampler.pull(chunk.data(), 8192u);
        output.insert(output.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(produced * 2u));
        if (produced != 0u) continue;
        if (offset == frames) break;
        offset += resampler.push(input.data() + offset * 2u, frames - offset);
    }
    return output;
}

bool test_resampler_pitch() {
    const u32 rates[] = {44100u, 48000u, 96000u, 22050u};
    const u64 input_frames = 10u * kSourceFramesPerSecond;
    std::vector<s16> input(static_cast<std::size_t>(input_frames) * 2u);
    for (u64 n = 0; n < input_frames; ++n) {
        const double value = 16384.0 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(n) / kSourceHz);
        input[n * 2u] = input[n * 2u + 1u] = static_cast<s16>(std::lrint(value));
    }
    for (u32 rate : rates) {
        AudioResampler resampler;
        CHECK(resampler.configure(gekko::kAudioSourceRateNum, gekko::kAudioSourceRateDen, rate));
        const std::vector<float> output = resample_all(resampler, input);
        const std::size_t frames = output.size() / 2u;
        double first = -1.0;
        double last = -1.0;
        u64 crossings = 0;
        for (std::size_t i = 1000u; i + 1u < frames - 1000u; ++i) {
            const double a = output[i * 2u];
            const double b = output[(i + 1u) * 2u];
            if (a < 0.0 && b >= 0.0) {
                const double t = static_cast<double>(i) + a / (a - b);
                if (first < 0.0) first = t;
                last = t;
                ++crossings;
            }
        }
        const double measured = static_cast<double>(crossings - 1u) * rate / (last - first);
        const double error = (measured - 1000.0) / 1000.0;
        std::printf("    %6u Hz out: 1 kHz in -> %.6f Hz (%+.3f ppm, %llu cycles)\n", rate, measured, error * 1e6,
                    static_cast<unsigned long long>(crossings));
        CHECK(std::fabs(error) < 1e-4);  // 0.01 %
    }
    return true;
}

double kaiser_window_amplitude(const std::vector<float>& interleaved, std::size_t start, std::size_t length,
                               double rate, double hz) {
    static std::vector<double> window;
    static double window_sum = 0.0;
    if (window.size() != length) {
        // Kaiser beta 20: sidelobes near -150 dB, so a -90 dB image beside a
        // full-scale tone is measured, not leaked.
        const auto i0 = [](double x) {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 500; ++k) {
                term *= (x * x / 4.0) / (static_cast<double>(k) * k);
                sum += term;
                if (term < sum * 1e-17) break;
            }
            return sum;
        };
        window.resize(length);
        window_sum = 0.0;
        const double beta = 20.0;
        for (std::size_t n = 0; n < length; ++n) {
            const double q = 2.0 * static_cast<double>(n) / static_cast<double>(length - 1u) - 1.0;
            window[n] = i0(beta * std::sqrt(std::max(0.0, 1.0 - q * q))) / i0(beta);
            window_sum += window[n];
        }
    }
    double re = 0.0;
    double im = 0.0;
    for (std::size_t n = 0; n < length; ++n) {
        const double angle = 2.0 * kPi * hz * static_cast<double>(n) / rate;
        const double v = window[n] * interleaved[(start + n) * 2u];
        re += v * std::cos(angle);
        im += v * std::sin(angle);
    }
    return 2.0 * std::sqrt(re * re + im * im) / window_sum;
}

bool test_resampler_response() {
    AudioResampler resampler;
    CHECK(resampler.configure(gekko::kAudioSourceRateNum, gekko::kAudioSourceRateDen, 48000u));
    // Analytic: the realized kernel (normalised rows + linear phase interpolation).
    double passband_dev = 0.0;
    for (double hz = 0.0; hz <= 14000.0; hz += 10.0) {
        passband_dev = std::max(passband_dev, std::fabs(20.0 * std::log10(resampler.response(hz))));
    }
    const double nyquist = kSourceHz / 2.0;
    double stopband = 1e9;
    double worst_hz = 0.0;
    for (double hz = nyquist; hz <= 96000.0; hz += 10.0) {
        const double attenuation = -20.0 * std::log10(resampler.response(hz));
        if (attenuation < stopband) {
            stopband = attenuation;
            worst_hz = hz;
        }
    }
    const double at_16k = -20.0 * std::log10(resampler.response(nyquist));
    std::printf("    kernel: %u taps x %u phases, cutoff %.1f Hz\n", resampler.taps(), AudioResampler::kPhases,
                resampler.cutoff_hz());
    std::printf("    analytic passband 0..14000 Hz: max deviation %.4f dB (limit 0.1)\n", passband_dev);
    std::printf("    analytic stopband %.1f..96000 Hz: min attenuation %.2f dB at %.0f Hz (limit 60); %.2f dB at Nyquist\n",
                nyquist, stopband, worst_hz, at_16k);
    CHECK(passband_dev <= 0.1);
    CHECK(stopband >= 60.0);

    // Measured through the streaming resampler at 96 kHz (images up to 48 kHz visible).
    const u32 rate = 96000u;
    const std::size_t window = 65536u;
    const auto tone = [&](double hz) {
        AudioResampler r;
        r.configure(gekko::kAudioSourceRateNum, gekko::kAudioSourceRateDen, rate);
        const u64 frames = static_cast<u64>((window + 4000u) * kSourceHz / rate) + 200u;
        std::vector<s16> input(static_cast<std::size_t>(frames) * 2u);
        for (u64 n = 0; n < frames; ++n) {
            const double value = 16384.0 * std::sin(2.0 * kPi * hz * static_cast<double>(n) / kSourceHz);
            input[n * 2u] = input[n * 2u + 1u] = static_cast<s16>(std::lrint(value));
        }
        return resample_all(r, input);
    };
    double worst_gain = 0.0;
    for (double hz : {1000.0, 5000.0, 10000.0, 14000.0}) {
        const std::vector<float> out = tone(hz);
        const double amplitude = kaiser_window_amplitude(out, 2000u, window, rate, hz);
        const double gain_db = 20.0 * std::log10(amplitude / 0.5);
        worst_gain = std::max(worst_gain, std::fabs(gain_db));
        std::printf("    measured gain %5.0f Hz: %+.4f dB\n", hz, gain_db);
    }
    CHECK(worst_gain <= 0.1);
    double worst_image = 1e9;
    for (double hz : {1000.0, 8000.0, 12000.0, 14000.0, 15500.0}) {
        const std::vector<float> out = tone(hz);
        const double below = kSourceHz - hz;
        const double above = kSourceHz + hz;
        const double image_below = -20.0 * std::log10(kaiser_window_amplitude(out, 2000u, window, rate, below) / 0.5);
        double image_above = 1e9;
        if (above < rate / 2.0) {
            image_above = -20.0 * std::log10(kaiser_window_amplitude(out, 2000u, window, rate, above) / 0.5);
        }
        worst_image = std::min({worst_image, image_below, image_above});
        std::printf("    measured image of %5.0f Hz: %.1f Hz -%.1f dB", hz, below, image_below);
        if (above < rate / 2.0) std::printf(", %.1f Hz -%.1f dB", above, image_above);
        std::printf("\n");
    }
    CHECK(worst_image >= 60.0);
    return true;
}

bool test_resampler_chunking_bit_identity() {
    const u32 rates[] = {48000u, 44100u, 96000u, 22050u};
    Rng noise(3);
    const std::size_t frames = 300000u;
    std::vector<s16> input(frames * 2u);
    for (s16& v : input) v = static_cast<s16>(noise.next());
    for (u32 rate : rates) {
        AudioResampler one_shot;
        CHECK(one_shot.configure(gekko::kAudioSourceRateNum, gekko::kAudioSourceRateDen, rate));
        const std::vector<float> reference = resample_all(one_shot, input);
        CHECK(reference.size() / 2u == expected_output_frames(frames, one_shot));

        const auto streamed = [&](u64 seed, u64 max_push, u64 max_pull) {
            AudioResampler r;
            r.configure(gekko::kAudioSourceRateNum, gekko::kAudioSourceRateDen, rate);
            std::vector<float> out;
            std::vector<float> chunk(max_pull * 2u);
            Rng rng(seed);
            std::size_t offset = 0;
            u64 mismatched_counts = 0;
            for (;;) {
                if (offset < frames && rng.range(0, 1) == 0u) {
                    const std::size_t want = std::min<std::size_t>(static_cast<std::size_t>(rng.range(1, max_push)),
                                                                   frames - offset);
                    offset += r.push(input.data() + offset * 2u, want);
                }
                const std::size_t want = static_cast<std::size_t>(rng.range(1, max_pull));
                const std::size_t made = r.pull(chunk.data(), want);
                out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(made * 2u));
                if (made < want && out.size() / 2u != expected_output_frames(offset, r)) ++mismatched_counts;
                if (offset == frames && made == 0u) break;
            }
            CHECK(mismatched_counts == 0u);
            return out;
        };
        const std::vector<float> random = streamed(17u + rate, 997u, 1500u);
        const std::vector<float> single = streamed(23u + rate, 1u, 1u);
        const bool same_random = random.size() == reference.size() &&
                                 std::memcmp(random.data(), reference.data(), reference.size() * sizeof(float)) == 0;
        const bool same_single = single.size() == reference.size() &&
                                 std::memcmp(single.data(), reference.data(), reference.size() * sizeof(float)) == 0;
        CHECK(same_random);
        CHECK(same_single);
        // FNV-1a of the output: equal across /arch builds when the kernel keeps
        // one numeric model (compare the SSE2 and AVX2 runs).
        u64 hash = 0xcbf29ce484222325ull;
        const u8* bytes = reinterpret_cast<const u8*>(reference.data());
        for (std::size_t i = 0; i < reference.size() * sizeof(float); ++i) {
            hash = (hash ^ bytes[i]) * 0x100000001b3ull;
        }
        std::printf("    %6u Hz: %zu output frames; random chunks %s, 1-frame chunks %s; output fnv1a %016llx\n", rate,
                    reference.size() / 2u, same_random ? "bit-identical" : "DIFFER",
                    same_single ? "bit-identical" : "DIFFER", static_cast<unsigned long long>(hash));
    }
    return true;
}

bool test_resampler_rational_frame_count() {
    const u32 rates[] = {48000u, 44100u, 96000u, 22050u};
    const u64 total = 100000000u;  // 10^8 input frames, ~52 min of guest audio
    std::vector<s16> zeros(4096u * 2u, 0);
    for (u32 rate : rates) {
        AudioResampler resampler;
        CHECK(resampler.configure(gekko::kAudioSourceRateNum, gekko::kAudioSourceRateDen, rate));
        Rng rng(rate);
        u64 pushed = 0;
        u64 produced = 0;
        u64 mismatches = 0;
        double max_offset = 0.0;
        while (pushed < total) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<u64>({rng.range(1, 4096), total - pushed, resampler.input_room()}));
            pushed += resampler.push(zeros.data(), want);
            // Timing only: the same clock stepping as a real pull.
            produced += resampler.pull(nullptr, std::numeric_limits<u64>::max());
            if (produced != expected_output_frames(pushed, resampler)) ++mismatches;
            const double ideal = static_cast<double>(pushed) * static_cast<double>(resampler.up()) /
                                 static_cast<double>(resampler.down());
            max_offset = std::max(max_offset, std::fabs(ideal - static_cast<double>(produced)));
        }
        const double ideal = static_cast<double>(total) * rate / kSourceHz;
        std::printf("    %6u Hz (%llu/%llu): %llu in -> %llu out, ideal %.3f, lag %.3f frames (bound %.3f), mismatches %llu\n",
                    rate, static_cast<unsigned long long>(resampler.up()),
                    static_cast<unsigned long long>(resampler.down()), static_cast<unsigned long long>(total),
                    static_cast<unsigned long long>(produced), ideal, ideal - static_cast<double>(produced),
                    resampler.half_taps() * static_cast<double>(resampler.up()) / resampler.down() + 1.0,
                    static_cast<unsigned long long>(mismatches));
        CHECK(mismatches == 0u);
        CHECK(max_offset <= resampler.half_taps() * static_cast<double>(resampler.up()) / resampler.down() + 1.0);
    }
    return true;
}

bool test_host_audio_64bit_indices() {
    using gekko::hw::AudioByteOffset64;
    using gekko::hw::AudioFrameIndex64;
    using gekko::hw::DmaCounter64;
    using gekko::hw::GuestAudioAddress32;
    using gekko::hw::HostAudioPointer64;
    using gekko::hw::SampleCounter64;
    CHECK(sizeof(GuestAudioAddress32) == 4u);
    CHECK(sizeof(HostAudioPointer64) == 8u);
    CHECK(sizeof(SampleCounter64) == 8u);
    CHECK(sizeof(DmaCounter64) == 8u);
    CHECK(sizeof(AudioByteOffset64) == 8u);
    CHECK(sizeof(AudioFrameIndex64) == 8u);

    AudioResampler resampler;
    CHECK(resampler.configure(gekko::kAudioSourceRateNum, gekko::kAudioSourceRateDen, 48000u));
    std::vector<s16> zeros(256u * 2u, 0);
    CHECK(resampler.push(zeros.data(), 256u) == 256u);
    // A 32-bit (n-1)*down wrap would make input_needed return 0 or a tiny
    // remainder, and the consumer would emit silence while the ring still
    // holds samples. Saturate to the held window instead.
    const AudioFrameIndex64 need = resampler.input_needed(std::numeric_limits<u64>::max());
    CHECK(need == resampler.input_room());
    CHECK(need != 0u);
    const AudioFrameIndex64 produced = resampler.pull(nullptr, std::numeric_limits<u64>::max());
    CHECK(produced == expected_output_frames(256u, resampler));
    std::printf("    types 32/64: guest addr %zu host ptr %zu sample %zu dma %zu byte %zu frame %zu; "
                "input_needed(2^64-1)=%llu pulled=%llu (pitch/ratio unchanged)\n",
                sizeof(GuestAudioAddress32), sizeof(HostAudioPointer64), sizeof(SampleCounter64),
                sizeof(DmaCounter64), sizeof(AudioByteOffset64), sizeof(AudioFrameIndex64),
                static_cast<unsigned long long>(need), static_cast<unsigned long long>(produced));
    return true;
}

// ---------------------------------------------------------------------------
// --device: a manual check against the real endpoint
// ---------------------------------------------------------------------------

void print_report(const AudioRuntime::Report& r) {
    const auto u = [](u64 v) { return static_cast<unsigned long long>(v); };
    std::printf("  device: \"%s\" %s %u Hz %u ch, period %llu frames (%.2f ms, %s), buffer %llu frames, mmcss=%d\n",
                r.device_name.c_str(), r.device_format.c_str(), r.device_rate, r.device_channels,
                u(r.device_period_frames), r.device_period_ms, r.low_latency_period ? "IAudioClient3 minimum" : "IAudioClient",
                u(r.device_buffer_frames), r.mmcss ? 1 : 0);
    std::printf("  resampler: %llu/%llu, %u taps; source %.4f Hz\n", u(r.resampler_up), u(r.resampler_down),
                r.resampler_taps, r.source_rate_hz);
    std::printf("  producer: blocks=%llu produced=%llu silence=%llu dropped=%llu (%llu events) resets=%llu\n",
                u(r.blocks), u(r.produced_frames), u(r.silence_frames), u(r.dropped_frames), u(r.dropped_events),
                u(r.reset_epochs));
    std::printf("  ring: capacity=%llu fill=%llu max_fill=%llu prime=%llu start_fill=%llu\n",
                u(r.ring_capacity_frames), u(r.ring_fill_frames), u(r.ring_max_fill_frames), u(r.prime_frames),
                u(r.start_fill_frames));
    std::printf("  endpoint: Start() %llu us, Start -> first event %llu us\n", u(r.device_start_us),
                u(r.first_wake_us));
    std::printf("  consumer: consumed=%llu output=%llu underruns=%llu (%llu frames) prime_silence=%llu clipped=%llu\n",
                u(r.consumed_frames), u(r.output_frames), u(r.underrun_events), u(r.underrun_frames),
                u(r.prime_silence_frames), u(r.clipped_samples));
    std::printf("  wakes=%llu timeouts=%llu late=%llu max_interval=%llu us max_render=%llu us starts=%llu\n",
                u(r.wakes), u(r.wake_timeouts), u(r.late_wakes), u(r.max_wake_interval_us), u(r.max_render_us),
                u(r.starts));
    std::printf("  reopens=%llu open_failures=%llu invalidations=%llu default_changes=%llu render_errors=%llu gap_discarded=%llu\n",
                u(r.reopens), u(r.open_failures), u(r.device_invalidations), u(r.default_device_changes),
                u(r.render_errors), u(r.device_gap_discarded_frames));
    std::printf("  fill histogram (5 ms buckets):");
    for (std::size_t i = 0; i < r.fill_histogram.size(); ++i) {
        if (r.fill_histogram[i] != 0u) std::printf(" [%zu-%zu ms]=%llu", i * 5u, i * 5u + 5u, u(r.fill_histogram[i]));
    }
    std::printf("\n  last_error: \"%s\"\n", r.last_error.c_str());
}

// The game's producer shape: 32-byte DMA blocks paced to wall time.
// amplitude 0 publishes zeros.
void pace_blocks(AudioRuntime& runtime, double seconds, double amplitude) {
    const auto begin = Clock::now();
    u64 produced = 0;
    u8 block[32];
    for (;;) {
        const double elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
        if (elapsed >= seconds) break;
        const u64 target = static_cast<u64>(elapsed * kSourceHz);
        while (produced + 8u <= target) {
            for (u64 k = 0; k < 8u; ++k) {
                const double t = static_cast<double>(produced + k) / kSourceHz;
                const s16 v = static_cast<s16>(std::lrint(amplitude * 32767.0 * std::sin(2.0 * kPi * 440.0 * t)));
                encode(block + k * 4u, v, v);
            }
            runtime.publish_block(block, sizeof(block));
            produced += 8u;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

u64 wakes_above_ms(const AudioRuntime::Report& r, u64 ms) {
    u64 count = 0;
    for (std::size_t i = static_cast<std::size_t>(ms / AudioRuntime::Report::kFillBucketMs); i < r.fill_histogram.size(); ++i) {
        count += r.fill_histogram[i];
    }
    return count;
}

#if defined(_WIN32)
// Wasapi mode with no usable endpoint, through fault injection: the injected
// failure returns before any device is touched, so no audio device is needed.
bool test_wasapi_no_endpoint() {
    {
        // A first open that fails while open() waits: false, Null, counting only.
        AudioRuntime runtime;
        AudioRuntime::Config config;
        config.fault_failed_opens = 1000000u;
        const bool opened = runtime.open(config);
        AudioRuntime::Report r = runtime.report();
        CHECK(!opened);
        CHECK(runtime.mode() == AudioRuntime::Mode::Null);
        CHECK(r.open_failures == 1u && !r.open_pending_at_return && !r.device_open);
        CHECK(r.ring_capacity_frames == 0u);
        CHECK(r.last_error.find("injected") != std::string::npos);
        u8 block[32] = {};
        runtime.publish_block(block, sizeof(block));
        r = runtime.report();
        CHECK(r.produced_frames == 8u && r.dropped_frames == 0u);
        std::printf("    failed first open, open() waiting: open=%d mode=%d open_failures=%llu last_error=\"%s\"\n",
                    opened ? 1 : 0, static_cast<int>(r.mode), static_cast<unsigned long long>(r.open_failures),
                    r.last_error.c_str());
    }
    {
        // ES-W7: the same failed first open with the handshake deferred. open()
        // returns at once (live, pending); wait_open() then gives exactly the
        // undeferred outcome: false, Null, counting only, the same error.
        AudioRuntime runtime;
        AudioRuntime::Config config;
        config.fault_failed_opens = 1000000u;
        config.fault_first_open_delay_ms = 100u;  // the first open is still running when open() returns
        config.defer_handshake = true;
        const auto open_begin = Clock::now();
        const bool opened = runtime.open(config);
        const double open_ms = std::chrono::duration<double, std::milli>(Clock::now() - open_begin).count();
        CHECK(opened && runtime.mode() == AudioRuntime::Mode::Wasapi && runtime.report().open_pending_at_return);
        CHECK(open_ms < 90.0);  // did not wait for the injected 100 ms
        const bool live = runtime.wait_open();
        AudioRuntime::Report r = runtime.report();
        CHECK(!live);
        CHECK(runtime.mode() == AudioRuntime::Mode::Null);
        CHECK(r.open_failures == 1u && !r.open_pending_at_return && !r.device_open);
        CHECK(r.ring_capacity_frames == 0u);
        CHECK(r.last_error.find("injected") != std::string::npos);
        CHECK(runtime.wait_open() == false);  // nothing left to wait for: still Null
        u8 block[32] = {};
        runtime.publish_block(block, sizeof(block));
        CHECK(runtime.report().produced_frames == 8u);
        std::printf("    deferred handshake, failed first open: open=%d in %.1f ms, wait_open=%d mode=%d "
                    "open_failures=%llu\n",
                    opened ? 1 : 0, open_ms, live ? 1 : 0, static_cast<int>(r.mode),
                    static_cast<unsigned long long>(r.open_failures));
    }
    {
        // ES-W7: deferred, and the first open outlives the deadline: wait_open()
        // returns at the deadline fixed by open(), live and pending, like open().
        AudioRuntime runtime;
        AudioRuntime::Config config;
        config.open_timeout_ms = 50u;
        config.fault_first_open_delay_ms = 300u;
        config.fault_failed_opens = 1000000u;
        config.defer_handshake = true;
        CHECK(runtime.open(config));
        const auto wait_begin = Clock::now();
        const bool live = runtime.wait_open();
        const double wait_ms = std::chrono::duration<double, std::milli>(Clock::now() - wait_begin).count();
        CHECK(live && runtime.mode() == AudioRuntime::Mode::Wasapi);
        CHECK(runtime.report().open_pending_at_return);
        CHECK(wait_ms < 250.0);  // the 50 ms deadline, not the 300 ms open
        runtime.close();
        std::printf("    deferred handshake, open outliving the deadline: wait_open=%d after %.1f ms\n",
                    live ? 1 : 0, wait_ms);
    }
    {
        // Verifier findings 2-4: an open that outlives open_timeout_ms and then
        // fails. open() must report a live runtime, the consumer must keep
        // retrying instead of exiting, and with no endpoint the default
        // 512 ms ring must not overflow (retries are 1 s apart).
        AudioRuntime runtime;
        AudioRuntime::Config config;
        config.open_timeout_ms = 0u;
        config.fault_first_open_delay_ms = 200u;
        config.fault_failed_opens = 1000000u;
        const auto open_begin = Clock::now();
        const bool opened = runtime.open(config);
        const double open_ms = std::chrono::duration<double, std::milli>(Clock::now() - open_begin).count();
        CHECK(opened);
        CHECK(runtime.mode() == AudioRuntime::Mode::Wasapi);
        CHECK(runtime.report().open_pending_at_return);
        pace_blocks(runtime, 1.5, 0.0);  // ~3 ring capacities of producer time
        const AudioRuntime::Report r = runtime.report();
        CHECK(r.mode == AudioRuntime::Mode::Wasapi && !r.device_open);
        CHECK(r.open_failures >= 2u);  // alive past its failed first open
        CHECK(r.dropped_frames == 0u && r.dropped_events == 0u);
        CHECK(r.device_gap_discarded_frames + r.ring_fill_frames == r.produced_frames);
        CHECK(r.produced_frames > 40000u);
        const auto close_begin = Clock::now();
        runtime.close();
        const double close_ms = std::chrono::duration<double, std::milli>(Clock::now() - close_begin).count();
        CHECK(close_ms < 500.0);
        std::printf("    open outliving its timeout, then failing: open=%d in %.1f ms, pending=%d; after 1.5 s "
                    "open_failures=%llu produced=%llu gap_discarded=%llu dropped=%llu/%llu max_fill=%llu of %llu; "
                    "close %.1f ms\n",
                    opened ? 1 : 0, open_ms, r.open_pending_at_return ? 1 : 0,
                    static_cast<unsigned long long>(r.open_failures), static_cast<unsigned long long>(r.produced_frames),
                    static_cast<unsigned long long>(r.device_gap_discarded_frames),
                    static_cast<unsigned long long>(r.dropped_frames), static_cast<unsigned long long>(r.dropped_events),
                    static_cast<unsigned long long>(r.ring_max_fill_frames),
                    static_cast<unsigned long long>(r.ring_capacity_frames), close_ms);
    }
    {
        // A small ring (4096 frames, 128 ms) drains every 32 ms with no endpoint.
        AudioRuntime runtime;
        AudioRuntime::Config config;
        config.ring_frames = 4096u;
        config.open_timeout_ms = 0u;
        config.fault_first_open_delay_ms = 20u;
        config.fault_failed_opens = 1000000u;
        CHECK(runtime.open(config));
        pace_blocks(runtime, 0.8, 0.0);
        const AudioRuntime::Report r = runtime.report();
        CHECK(r.dropped_frames == 0u);
        CHECK(r.device_gap_discarded_frames + r.ring_fill_frames == r.produced_frames);
        std::printf("    ring 4096, no endpoint, 0.8 s: produced=%llu gap_discarded=%llu dropped=%llu max_fill=%llu\n",
                    static_cast<unsigned long long>(r.produced_frames),
                    static_cast<unsigned long long>(r.device_gap_discarded_frames),
                    static_cast<unsigned long long>(r.dropped_frames),
                    static_cast<unsigned long long>(r.ring_max_fill_frames));
    }
    return true;
}
#endif

struct DeviceArm {
    bool opened = false;
    double open_ms = 0.0;
    AudioRuntime::Report report;
};

DeviceArm run_device_arm(const char* name, const AudioRuntime::Config& config, double seconds, double amplitude) {
    DeviceArm arm;
    AudioRuntime runtime;
    const auto open_begin = Clock::now();
    arm.opened = runtime.open(config);
    arm.open_ms = std::chrono::duration<double, std::milli>(Clock::now() - open_begin).count();
    std::printf("[%s] open() = %d in %.1f ms, mode %d\n", name, arm.opened ? 1 : 0, arm.open_ms,
                static_cast<int>(runtime.mode()));
    if (arm.opened) pace_blocks(runtime, seconds, amplitude);
    arm.report = runtime.report();
    std::printf("[%s] after %.1f s of production (%s):\n", name, seconds, amplitude > 0.0 ? "440 Hz tone" : "zeros");
    print_report(arm.report);
    std::printf("[%s] wakes with fill above 100 ms: %llu\n", name,
                static_cast<unsigned long long>(wakes_above_ms(arm.report, 100u)));
    runtime.close();
    return arm;
}

int device_check(bool condition, const char* text) {
    std::printf("  %s %s\n", condition ? "ok  " : "FAIL", text);
    return condition ? 0 : 1;
}

int run_device_tests(bool late_arm) {
    int failures = 0;
    {
        AudioRuntime::Config config;
        const DeviceArm arm = run_device_arm("A default", config, 2.0, 0.1);
        failures += device_check(arm.opened && arm.report.device_open, "A: endpoint open and running");
        failures += device_check(arm.report.dropped_frames == 0u, "A: no drops");
        failures += device_check(wakes_above_ms(arm.report, 100u) <= 2u, "A: steady fill near the prime");
    }
    {
        // Verifier finding 2: open() returns at its timeout while Start() runs.
        AudioRuntime::Config config;
        config.open_timeout_ms = 0u;
        config.fault_first_open_delay_ms = 150u;
        const DeviceArm arm = run_device_arm("B open past timeout", config, 2.0, 0.0);
        failures += device_check(arm.opened && arm.report.open_pending_at_return, "B: open() true and pending");
        failures += device_check(arm.report.device_open && arm.report.starts >= 1u, "B: endpoint playing");
        failures += device_check(arm.report.device_gap_discarded_frames > 0u, "B: the Start() pile was trimmed");
        failures += device_check(arm.report.dropped_frames == 0u, "B: no drops");
        failures += device_check(wakes_above_ms(arm.report, 100u) <= 2u, "B: steady fill near the prime, not ~190 ms");
    }
    {
        AudioRuntime::Config config;
        config.fault_invalidate_after_wakes = 50u;
        const DeviceArm arm = run_device_arm("C invalidate every 50 wakes", config, 2.5, 0.0);
        const AudioRuntime::Report& r = arm.report;
        failures += device_check(arm.opened && r.device_invalidations >= 2u, "C: invalidations injected");
        failures += device_check(r.reopens + 1u >= r.device_invalidations && r.reopens >= 1u,
                                 "C: every invalidation reopened (the last may be in progress)");
        failures += device_check(r.open_failures == 0u && r.dropped_frames == 0u, "C: no open failures, no drops");
    }
    {
        // Verifier finding 3: the first open fails after open() returned; the
        // consumer must retry and open the real endpoint a second later.
        AudioRuntime::Config config;
        config.open_timeout_ms = 0u;
        config.fault_first_open_delay_ms = 150u;
        config.fault_failed_opens = 1u;
        const DeviceArm arm = run_device_arm("D first open fails past timeout", config, 2.5, 0.0);
        const AudioRuntime::Report& r = arm.report;
        failures += device_check(arm.opened && r.open_pending_at_return, "D: open() true and pending");
        failures += device_check(r.open_failures == 1u && r.device_open && r.reopens == 0u,
                                 "D: one failure, then the endpoint opened (not a reopen)");
        failures += device_check(r.dropped_frames == 0u, "D: no drops while no endpoint existed");
        failures += device_check(wakes_above_ms(r, 100u) <= 2u, "D: steady fill near the prime");
    }
    if (late_arm) {
        AudioRuntime::Config config;
        config.start_device_at_open = false;
        run_device_arm("E start after prime (A/B)", config, 2.0, 0.0);
    }
    std::printf("device checks: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace

// Offline diagnostic: use the production kernel, rational clock and identical
// source words. No device, priming, rate servo, fades or synthesized end padding.
// Input is bounded to ten minutes; processing uses small reusable buffers.
int export_resampled_source(const char* input_path, const char* output_path, const char* rate_text) {
    char* end = nullptr;
    const unsigned long rate = std::strtoul(rate_text, &end, 10);
    if (!end || *end || rate < 8000u || rate > 384000u) {
        std::fprintf(stderr, "output rate must be 8000..384000\n");
        return 2;
    }
    std::error_code error;
    const auto bytes = std::filesystem::file_size(input_path, error);
    constexpr u64 max_frames = kSourceFramesPerSecond * 600ull;
    if (error || bytes % 4u != 0u || bytes / 4u > max_frames) {
        std::fprintf(stderr, "input must be complete S16BE stereo frames, at most ten minutes\n");
        return 2;
    }
    const u32 endian_probe = 1;
    if (*reinterpret_cast<const u8*>(&endian_probe) != 1u || sizeof(float) != 4u) return 2;
    std::ifstream source(input_path, std::ios::binary);
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    AudioResampler resampler;
    if (!source || !output || !resampler.configure(gekko::kAudioSourceRateNum,
                                                   gekko::kAudioSourceRateDen, static_cast<u32>(rate))) return 2;
    std::vector<u8> encoded(1024u * 4u);
    std::vector<s16> input(1024u * 2u);
    std::vector<float> generated(4096u * 2u);
    u64 input_frames = 0, output_frames = 0;
    const auto drain = [&]() {
        u64 count = 0;
        for (;;) {
            const auto made = resampler.pull(generated.data(), 4096u);
            if (!made) break;
            output.write(reinterpret_cast<const char*>(generated.data()),
                         static_cast<std::streamsize>(made * 2u * sizeof(float)));
            count += made;
        }
        output_frames += count;
        return count;
    };
    while (source) {
        source.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        const auto got = static_cast<std::size_t>(source.gcount());
        if (got % 4u != 0u) return 2;
        if (!got) break;
        const auto frames = got / 4u;
        for (std::size_t i = 0; i < frames * 2u; ++i) {
            const u16 word = static_cast<u16>(encoded[2u * i]) << 8u | encoded[2u * i + 1u];
            std::memcpy(&input[i], &word, sizeof(word));
        }
        std::size_t used = 0;
        while (used < frames) {
            const auto pushed = resampler.push(input.data() + used * 2u, frames - used);
            used += static_cast<std::size_t>(pushed);
            if (!drain() && !pushed) return 2;
        }
        input_frames += frames;
    }
    drain();
    output.flush();
    if (!source.eof() || !output || input_frames * 4u != bytes) return 2;
    std::printf("RESAMPLED_SOURCE input_frames=%llu output_frames=%llu rate=%lu ratio=%llu/%llu taps=%u half_taps=%u format=float32le-stereo end_padding=none\n",
                input_frames, output_frames, rate, resampler.up(), resampler.down(),
                resampler.taps(), resampler.half_taps());
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--resample-source") == 0) {
        if (argc != 5) {
            std::fprintf(stderr, "--resample-source input.s16be output.f32le output-rate\n");
            return 2;
        }
        return export_resampled_source(argv[2], argv[3], argv[4]);
    }
    if (argc > 1 && std::strcmp(argv[1], "--device") == 0) {
        // --device: a quiet 440 Hz tone on the default configuration, then
        // silent arms for the open-timeout, invalidation and failed-first-open
        // paths. --device late adds the start-after-prime arm.
        return run_device_tests(argc > 2 && std::strcmp(argv[2], "late") == 0);
    }

    struct Case {
        const char* name;
        bool (*run)();
    };
    const Case cases[] = {
        {"ring_spsc_stress_1e7", test_ring_spsc_stress},
        {"overflow_accounting", test_overflow_accounting},
        {"partial_sample_carry", test_partial_sample_carry},
        {"epoch_reset", test_epoch_reset},
        {"concurrent_epoch_reset", test_concurrent_epoch_reset},
        {"null_mode", test_null_mode},
        {"prime_underrun_resume", test_prime_underrun_resume},
        {"prime_under_lead", test_prime_under_lead},
        {"underrun_event_per_gap", test_underrun_event_per_gap},
        {"finish_production_tail", test_finish_production_tail},
        {"device_formats", test_device_formats},
        {"render_chunking_bit_identity", test_render_chunking_bit_identity},
        {"fill_histogram", test_fill_histogram},
        {"resampler_pitch", test_resampler_pitch},
        {"resampler_response", test_resampler_response},
        {"resampler_chunking_bit_identity", test_resampler_chunking_bit_identity},
        {"resampler_rational_frame_count_1e8", test_resampler_rational_frame_count},
        {"host_audio_64bit_indices", test_host_audio_64bit_indices},
#if defined(_WIN32)
        {"wasapi_no_endpoint_fault_injection", test_wasapi_no_endpoint},
#endif
    };
    int failed = 0;
    for (const Case& c : cases) {
        const int before = g_check_failures;
        const auto begin = Clock::now();
        std::printf("RUN  %s\n", c.name);
        std::fflush(stdout);
        const bool ok = c.run() && g_check_failures == before;
        std::printf("%s %s (%.2f s)\n", ok ? "PASS" : "FAIL", c.name,
                    std::chrono::duration<double>(Clock::now() - begin).count());
        std::fflush(stdout);
        if (!ok) ++failed;
    }
    std::printf("%d of %zu cases failed\n", failed, sizeof(cases) / sizeof(cases[0]));
    return failed == 0 ? 0 : 1;
}
