// The room reverb's block output, pinned bit for bit.
//
// shadow::audio::Reverb::process has a scalar reference path and, in AVX2 builds, a path that runs the three channels
// in vector lanes. The hashes below were produced by the scalar path (the arithmetic the original instruction oracle,
// axfx_reverb_contract_tests, proves against the pinned DOL) over deterministic signals; every build, scalar or
// vector, must reproduce them. A check that cannot fail is no check, so the last step breaks the input by one sample
// and requires the hash to move.
//
// Signals: loud noise, near-silence and exact silence (the tails decay through the subnormal range, where the flush
// mode's rules act), blocks of 160, 37 and 1 frames, one block of values near the int32 limits, and a set of
// restored histories whose first fused operation lands on the smallest-normal boundary of the flush rule.
#include "audio/reverb.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using shadow::audio::Reverb;
using shadow::math::SubnormalMode;

struct Hash {
    std::uint64_t value = 14695981039346656037ull;
    void add(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) { value ^= bytes[i]; value *= 1099511628211ull; }
    }
};

struct Scenario {
    const char* name;
    Reverb::Parameters parameters;
    SubnormalMode mode;
    std::uint64_t expected;
};

std::uint64_t run_signal(const Scenario& scenario, int tamper_block) {
    Reverb reverb;
    reverb.configure(scenario.parameters);
    Hash hash;
    std::uint64_t state = 88172645463325252ull;
    std::vector<std::int32_t> channel[3];
    for (auto& block : channel) block.resize(160);
    for (int block = 0; block < 1500; ++block) {
        const std::size_t frames = (block % 7 == 3) ? 37 : (block % 11 == 5 ? 1 : 160);
        for (int c = 0; c < 3; ++c) {
            for (std::size_t i = 0; i < frames; ++i) {
                state ^= state << 13; state ^= state >> 7; state ^= state << 17;
                std::int32_t value = (block % 50 < 30) ? static_cast<std::int32_t>(state >> 40) - (1 << 23)
                                                       : (block % 50 < 40 ? 0 : static_cast<std::int32_t>(state >> 60));
                if (block >= 100 && block < 400) value = 0;  // exact silence: the tails run down through the subnormals
                if (block == 1234) value = static_cast<std::int32_t>(state >> 33);  // near the int32 limits
                if (block == tamper_block && c == 1 && i == 0) value += 1;
                channel[c][i] = value;
            }
        }
        reverb.process({channel[0].data(), channel[1].data(), channel[2].data()}, frames, scenario.mode);
        for (int c = 0; c < 3; ++c) hash.add(channel[c].data(), frames * sizeof(std::int32_t));
        // Inside the silence the outputs are all zero; the state is where the subnormal rules show.
        if (block >= 100 && block < 400 && block % 25 == 0) hash.add(&reverb.history(), sizeof(Reverb::History));
    }
    hash.add(&reverb.history(), sizeof(Reverb::History));
    return hash.value;
}

// Histories whose delay cells (and so the first fused operations) sit around min_normal / coefficient, silent input.
// color 0.5 makes the product exact (the boundary result is kept: residual 0); 0.3 makes it inexact (the boundary
// result may round up from below the smallest normal and is then flushed).
std::uint64_t run_boundary(SubnormalMode mode) {
    constexpr float kMinNormal = 0x1p-126f;
    Hash hash;
    for (const float color : {0.3f, 0.5f}) {
        Reverb::Parameters p;
        p.color = color; p.time = 1.0f; p.pre_delay = 0.0f; p.crossfeed = 0.0f;
        const float duration = 32000.0f * p.time;
        const int lengths[3] = {1789, 1999, 2333};
        float comb[3];
        for (int line = 0; line < 3; ++line)
            comb[line] = float(std::pow(10.0, double(float(-3 * lengths[line]) / duration)));
        Reverb reverb;
        for (int step = -400; step <= 400; ++step) {
            reverb.configure(p);
            Reverb::History history = reverb.history();
            const float centre[6] = {kMinNormal / comb[0], kMinNormal / comb[1], kMinNormal / comb[2],
                                     kMinNormal / p.color, kMinNormal / p.color, kMinNormal / p.color};
            for (int channel = 0; channel < 3; ++channel) {
                for (int line = 0; line < 6; ++line) {
                    float value = centre[line] * (channel == 1 ? -1.0f : 1.0f);
                    // Walk `step` representable values away from the centre.
                    for (int k = 0; k < std::abs(step); ++k)
                        value = std::nextafter(value, step > 0 ? INFINITY : -INFINITY);
                    history.delayed[channel * 6 + line] = value;
                }
            }
            if (!reverb.restore(history)) { std::puts("boundary: restore refused a finite history"); return 0; }
            std::array<std::int32_t, 4> zero[3] = {};
            std::array<std::int32_t*, 3> channels{zero[0].data(), zero[1].data(), zero[2].data()};
            reverb.process(channels, 2, mode);
            for (auto& block : zero) hash.add(block.data(), 2 * sizeof(std::int32_t));
            hash.add(&reverb.history(), sizeof(Reverb::History));
        }
    }
    return hash.value;
}


// Histories restored from outside may hold subnormal values anywhere (cells, delayed, low-pass, previous input): the
// operand flush has to act on every one of them wherever the vector path uses it, and the stored history must keep
// the raw values the original keeps. Deterministic mixture of subnormal, tiny, ordinary and zero values, blocks of
// 0, 1, 3, 4, 5 and 160 frames (the vector crossfeed works in fours with a scalar tail).
std::uint64_t run_dirty(const Reverb::Parameters& p, SubnormalMode mode) {
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    const auto next = [&]() { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; };
    const auto value = [&]() -> float {
        const std::uint64_t r = next();
        switch (r & 7u) {
            case 0: return 0.0f;
            case 1: case 2: { std::uint32_t bits = static_cast<std::uint32_t>(r >> 8) & 0x007FFFFFu; if (r & 0x100000000ull) bits |= 0x80000000u; float f; std::memcpy(&f, &bits, 4); return f; }
            case 3: { std::uint32_t bits = 0x00800000u + (static_cast<std::uint32_t>(r >> 8) & 0xFFu); float f; std::memcpy(&f, &bits, 4); return (r & 0x100000000ull) ? -f : f; }
            case 4: return static_cast<float>(static_cast<std::int32_t>(r >> 40) - (1 << 23)) * 1e-30f;
            default: return static_cast<float>(static_cast<std::int32_t>(r >> 40) - (1 << 23)) * (1.0f / 64.0f);
        }
    };
    Hash hash;
    for (int round = 0; round < 40; ++round) {
        Reverb reverb;
        reverb.configure(p);
        Reverb::History history = reverb.history();
        for (float& s : history.samples) s = value();
        for (float& s : history.delayed) s = value();
        for (float& s : history.low_pass) s = value();
        for (float& s : history.previous_input) s = value();
        for (std::size_t line = 0; line < Reverb::kLines; ++line) history.positions[line] = static_cast<std::uint16_t>(next() % 47u);
        if (!reverb.restore(history)) { std::puts("dirty: restore refused a finite history"); return 0; }
        for (const std::size_t frames : {std::size_t{0}, std::size_t{1}, std::size_t{3}, std::size_t{4}, std::size_t{5}, std::size_t{160}, std::size_t{160}}) {
            std::array<std::int32_t, 160> block[3];
            for (auto& b : block) for (std::size_t i = 0; i < frames; ++i) b[i] = (round % 4 == 0) ? 0 : static_cast<std::int32_t>(next() >> 44) - (1 << 19);
            reverb.process({block[0].data(), block[1].data(), block[2].data()}, frames, mode);
            for (auto& b : block) hash.add(b.data(), frames * sizeof(std::int32_t));
            hash.add(&reverb.history(), sizeof(Reverb::History));
        }
    }
    return hash.value;
}

}  // namespace

int main(int argc, char** argv) {
    const bool print = argc > 1 && std::strcmp(argv[1], "--print") == 0;
    Reverb::Parameters live;
    Reverb::Parameters wide;
    wide.color = 0.9f; wide.level = 0.4f; wide.time = 2.5f; wide.damping = 0.2f; wide.pre_delay = 0.0f; wide.crossfeed = 0.0f;
    Reverb::Parameters fast;  // a 20 ms decay: the tails reach the subnormal range within a few blocks of silence
    fast.time = 0.02f; fast.damping = 0.0f; fast.color = 0.7f;
    const Scenario scenarios[] = {
        {"live parameters, flush", live, SubnormalMode::Flush, 0x39a4f3c4bbdbef6dull},
        {"live parameters, preserve", live, SubnormalMode::Preserve, 0x39a4f3c4bbdbef6dull},
        {"wide parameters, flush", wide, SubnormalMode::Flush, 0xb8dc74bac480a12aull},
        {"wide parameters, preserve", wide, SubnormalMode::Preserve, 0xb8dc74bac480a12aull},
        {"fast decay, flush", fast, SubnormalMode::Flush, 0x8952524baed1f5e0ull},
        {"fast decay, preserve", fast, SubnormalMode::Preserve, 0x8c284adc646e7ff7ull},
    };
    int failures = 0;
    for (const Scenario& scenario : scenarios) {
        const std::uint64_t got = run_signal(scenario, -1);
        const std::uint64_t broken = run_signal(scenario, 700);
        if (print) { std::printf("%-28s 0x%016llxull\n", scenario.name, static_cast<unsigned long long>(got)); continue; }
        const bool ok = got == scenario.expected;
        // Negative control: one input sample changed must change the hash.
        const bool control = broken != got;
        std::printf("%-28s %s  negative control %s\n", scenario.name, ok ? "ok" : "MISMATCH", control ? "caught" : "NOT CAUGHT");
        if (!ok) std::printf("  got 0x%016llx want 0x%016llx\n", static_cast<unsigned long long>(got), static_cast<unsigned long long>(scenario.expected));
        failures += (!ok) + (!control);
    }
    {
        Reverb::Parameters dirty_live;               // crossfeed 0.3, pre-delay 0.01: every stage of the vector path runs
        Reverb::Parameters dirty_plain; dirty_plain.pre_delay = 0.0f; dirty_plain.crossfeed = 0.0f; dirty_plain.color = 0.3f;
        const std::uint64_t dirty[4] = {run_dirty(dirty_live, SubnormalMode::Flush), run_dirty(dirty_live, SubnormalMode::Preserve),
                                        run_dirty(dirty_plain, SubnormalMode::Flush), run_dirty(dirty_plain, SubnormalMode::Preserve)};
        constexpr std::uint64_t kDirtyExpected[4] = {0xa2991549ba9b78f9ull, 0x84d62152ea8b94c3ull, 0xaaddfa29812817c2ull, 0xa2fef2964b0b1046ull};
        static const char* const names[4] = {"dirty live flush", "dirty live preserve", "dirty plain flush", "dirty plain preserve"};
        for (int i = 0; i < 4; ++i) {
            if (print) { std::printf("%-28s 0x%016llxull\n", names[i], static_cast<unsigned long long>(dirty[i])); continue; }
            const bool ok = dirty[i] == kDirtyExpected[i];
            std::printf("%-28s %s\n", names[i], ok ? "ok" : "MISMATCH");
            if (!ok) std::printf("  got 0x%016llx want 0x%016llx\n", static_cast<unsigned long long>(dirty[i]), static_cast<unsigned long long>(kDirtyExpected[i]));
            failures += !ok;
        }
    }
    const std::uint64_t boundary[2] = {run_boundary(SubnormalMode::Flush), run_boundary(SubnormalMode::Preserve)};
    constexpr std::uint64_t kBoundaryExpected[2] = {0x81c5c940ab7f2e26ull, 0x8a4182c0f3fd7f7eull};
    if (print) {
        std::printf("boundary flush               0x%016llxull\nboundary preserve            0x%016llxull\n",
                    static_cast<unsigned long long>(boundary[0]), static_cast<unsigned long long>(boundary[1]));
        return 0;
    }
    for (int i = 0; i < 2; ++i) {
        const bool ok = boundary[i] == kBoundaryExpected[i];
        std::printf("boundary %-19s %s\n", i == 0 ? "flush" : "preserve", ok ? "ok" : "MISMATCH");
        if (!ok) std::printf("  got 0x%016llx want 0x%016llx\n", static_cast<unsigned long long>(boundary[i]), static_cast<unsigned long long>(kBoundaryExpected[i]));
        failures += !ok;
    }
    std::puts(failures == 0 ? "reverb_block_hash_tests: ok" : "reverb_block_hash_tests: FAILED");
    return failures == 0 ? 0 : 1;
}
