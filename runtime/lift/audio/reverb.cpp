#include "audio/reverb.h"
#include "audio/reverb_ops.h"

#include <algorithm>
#include <cmath>
#if defined(__AVX2__) || defined(__FMA__)
#include <immintrin.h>
#endif

namespace shadow::audio {
namespace {
constexpr std::array<std::uint16_t, Reverb::kLines> kLength{{
    1789, 1999, 2333, 433, 149, 47,
    1789, 1999, 2333, 433, 149, 73,
    1789, 1999, 2333, 433, 149, 67}};
constexpr auto offsets() noexcept {
    std::array<std::size_t, Reverb::kLines + 1> result{};
    for (std::size_t line = 0; line < Reverb::kLines; ++line) result[line + 1] = result[line] + kLength[line];
    return result;
}
constexpr auto kOffset = offsets();
static_assert(kOffset.back() == Reverb::kDelaySamples);
constexpr float kMinNormal = 0x1p-126f;
using Mode = math::SubnormalMode;
constexpr Mode kFlush = Mode::Flush;
// The arithmetic helpers run a few dozen times per sample (18 lines x 160 frames x 3 channels per 5 ms frame). As
// out-of-line calls each testing the mode they were 0.12 ms of the game thread per frame (2026-11-04 profile: fused
// 0.055, nearest 0.011, add/mul 0.011 beside process 0.043); with the mode a template argument and the helpers
// inlined, every operation is the same arithmetic, written once, with no call and no mode test.
#if defined(_MSC_VER)
#define SHADOW_REVERB_INLINE __forceinline
#else
#define SHADOW_REVERB_INLINE inline __attribute__((always_inline))
#endif

bool valid(const Reverb::Parameters& p) noexcept {
    return std::isfinite(p.color) && std::isfinite(p.level) && std::isfinite(p.time) &&
           std::isfinite(p.damping) && std::isfinite(p.pre_delay) && std::isfinite(p.crossfeed) &&
           p.color >= 0 && p.color <= 1 && p.level >= 0 && p.level <= 1 && p.time >= 0.01f && p.time <= 10 &&
           p.damping >= 0 && p.damping <= 1 && p.pre_delay >= 0 && p.pre_delay <= 0.1f &&
           p.crossfeed >= 0 && p.crossfeed <= 1;
}

// Single-precision arithmetic. Parameters/history reject nonfinite values.
// Flush mode handles operands and exact underflow before the final rounding.
template <Mode M>
SHADOW_REVERB_INLINE float operand(float value) noexcept {
    return M == kFlush && std::fabs(value) < kMinNormal ? std::copysign(0.0f, value) : value;
}
template <Mode M>
SHADOW_REVERB_INLINE float rounded(double value) noexcept {
    return M == kFlush && std::fabs(value) < kMinNormal ? std::copysign(0.0f, float(value)) : float(value);
}
template <Mode M>
SHADOW_REVERB_INLINE float add(float a, float b) noexcept {
    return rounded<M>(double(operand<M>(a)) + double(operand<M>(b)));
}
template <Mode M>
SHADOW_REVERB_INLINE float mul(float a, float b) noexcept {
    return rounded<M>(double(operand<M>(a)) * double(operand<M>(b)));
}
template <Mode M>
SHADOW_REVERB_INLINE float fused(float a, float b, float c) noexcept {
    a = operand<M>(a); b = operand<M>(b); c = operand<M>(c);
#if defined(__AVX2__) || defined(__FMA__)
    const float value = _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c)));
#else
    const float value = std::fma(a, b, c);
#endif
    if constexpr (M != kFlush) return value;
    // Zero and subnormal rounded results already have the required sign.
    // Only the normal-boundary result needs the exact residual calculation.
    if (std::fabs(value) < kMinNormal) return std::copysign(0.0f, value);
    if (std::fabs(value) == kMinNormal) {
        const double product = double(a) * double(b);
        const double sum = product + double(c);
        const double moved = sum - product;
        const double residual = (product - (sum - moved)) + (double(c) - moved);
        if (std::fabs(sum) < kMinNormal || (std::fabs(sum) == kMinNormal &&
            (std::signbit(sum) ? residual > 0 : residual < 0))) return std::copysign(0.0f, value);
    }
    return value;
}
// The authored flush mode also maps every invalid integer conversion to
// INT32_MIN. Standard mode saturates positive overflow. Test the signal
// after conversion: +2^31 must not silently acquire the opposite sign.
template <Mode M>
SHADOW_REVERB_INLINE std::int32_t truncate(float value) noexcept {
    if (std::isnan(value)) return INT32_MIN;
    if (value >= 0x1p31f) return M == kFlush ? INT32_MIN : INT32_MAX;
    if (value <= -0x1p31f) return INT32_MIN;
    return static_cast<std::int32_t>(value);
}
template <Mode M>
SHADOW_REVERB_INLINE std::int32_t nearest(float value) noexcept {
    if (std::isnan(value)) return INT32_MIN;
    if (value >= 0x1p31f) return M == kFlush ? INT32_MIN : INT32_MAX;
    if (value <= -0x1p31f) return INT32_MIN;
    const auto integer = static_cast<std::int32_t>(value);
    const float fraction = value - float(integer);
    if (fraction > 0.5f || (fraction == 0.5f && (integer & 1))) return integer + 1;
    if (fraction < -0.5f || (fraction == -0.5f && (integer & 1))) return integer - 1;
    return integer;
}
} // namespace

Reverb::Reverb() noexcept { coefficients(); }

void Reverb::coefficients() noexcept {
    const auto& p = history_.parameters;
    const float duration = 32000.0f * p.time;
    for (std::size_t line = 0; line < comb_.size(); ++line) {
        const float exponent = float(-3 * int(kLength[line])) / duration;
        comb_[line] = float(std::pow(10.0, double(exponent)));
    }
    const float damped = 0.8f * std::max(p.damping, 0.05f);
    const float damping_sum = 0.05f + damped;
    damping_ = 1.0f - damping_sum;
    wet_ = p.level * 0.6f; dry_ = 0.6f - wet_;
    cross_ = 0.5f * p.crossfeed; direct_ = 1.0f - cross_;
    // Original initialized pre-delay reloads the same cursor each sample.
    // Preserve its observed one-sample signal, not its unused 320-float buffer.
    pre_delay_ = static_cast<unsigned>(32000.0f * p.pre_delay) != 0;
}

bool Reverb::configure(const Parameters& parameters) noexcept {
    if (!valid(parameters)) return false;
    history_.parameters = parameters; coefficients(); reset(); return true;
}
void Reverb::reset() noexcept {
    history_.samples.fill(0); history_.positions.fill(0); history_.delayed.fill(0);
    history_.low_pass.fill(0); history_.previous_input.fill(0);
}
bool Reverb::restore(const History& saved) noexcept {
    if (!valid(saved.parameters)) return false;
    for (std::size_t line = 0; line < kLines; ++line) if (saved.positions[line] >= kLength[line]) return false;
    const auto finite = [](const auto& values) { return std::all_of(values.begin(), values.end(),
        [](float value) { return std::isfinite(value); }); };
    if (!finite(saved.samples) || !finite(saved.delayed) || !finite(saved.low_pass) || !finite(saved.previous_input))
        return false;
    history_ = saved; coefficients(); return true;
}

SHADOW_REVERB_INLINE float Reverb::exchange(std::size_t line, float input) noexcept {
    auto& position = history_.positions[line];
    float& cell = history_.samples[kOffset[line] + position];
    const float output = cell;
    cell = input;
    if (++position == kLength[line]) position = 0;
    return output;
}

template <math::SubnormalMode M>
void Reverb::crossfeed(const std::array<std::int32_t*, 3>& channels, std::size_t frames) noexcept {
    std::size_t i = 0;
#if defined(__AVX2__)
    {
        // Four frames per step: the same products and sum (each product separately rounded, flushed as the scalar
        // helpers do), the sum rounded to the nearest integer, ties to even (cvtps2dq in the host's default rounding;
        // the scalar `nearest` computes the same integer for every in-range value, and gives INT32_MIN to NaN and to
        // the out-of-range values like cvtps2dq; the preserve mode's positive overflow is patched to INT32_MAX).
        const __m128 direct = vec::operand<M>(_mm_set1_ps(direct_));
        const __m128 cross = vec::operand<M>(_mm_set1_ps(cross_));
        const auto round_to_int = [](__m128 value) noexcept {
            const __m128i converted = _mm_cvtps_epi32(value);
            if constexpr (M == kFlush) return converted;
            const __m128 overflow = _mm_cmpge_ps(value, _mm_set1_ps(0x1p31f));
            return _mm_blendv_epi8(converted, _mm_set1_epi32(INT32_MAX), _mm_castps_si128(overflow));
        };
        for (; i + 4 <= frames; i += 4) {
            const __m128 left = _mm_cvtepi32_ps(_mm_loadu_si128(reinterpret_cast<const __m128i*>(channels[0] + i)));
            const __m128 right = _mm_cvtepi32_ps(_mm_loadu_si128(reinterpret_cast<const __m128i*>(channels[1] + i)));
            const __m128 first = vec::add_clean<M>(vec::mul_clean<M>(left, direct), vec::mul_clean<M>(right, cross));
            const __m128 second = vec::add_clean<M>(vec::mul_clean<M>(left, cross), vec::mul_clean<M>(right, direct));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(channels[0] + i), round_to_int(first));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(channels[1] + i), round_to_int(second));
        }
    }
#endif
    for (; i < frames; ++i) {
        const float left = float(channels[0][i]), right = float(channels[1][i]);
        // Paired crossfeed uses separately rounded products, not a fused sum.
        channels[0][i] = nearest<M>(add<M>(mul<M>(left, direct_), mul<M>(right, cross_)));
        channels[1][i] = nearest<M>(add<M>(mul<M>(left, cross_), mul<M>(right, direct_)));
    }
}

#if defined(__AVX2__)
// Lane k of every vector is channel k (lane 3 stays zero): the three channels run the same operations on their own
// state, so one vector operation does the work of three scalar ones. Each lane computes exactly what the scalar
// helpers above compute for that channel (same operand flush, same single rounding of the exact result, same fused
// multiply-add, same flush of the result); the helpers and the argument for dropping the operand flush on values the
// loop itself produced are in reverb_ops.h. The scalar path below stays as the reference for builds without AVX2 / FMA,
// and the block-hash test (reverb_block_hash_tests) pins the output to the same bits.

template <math::SubnormalMode M>
void Reverb::run_vector(const std::array<std::int32_t*, 3>& channels, std::size_t frames) noexcept {
    using vec::V;
    constexpr std::size_t kChannels = 3;
    // The delay lines' storage and cursors per (channel, line); the state and the coefficients as vectors.
    std::array<float*, kLines> cell_base{};
    std::array<std::uint32_t, kLines> position{};
    for (std::size_t index = 0; index < kLines; ++index) {
        cell_base[index] = history_.samples.data() + kOffset[index];
        position[index] = history_.positions[index];
    }
    const auto load_state = [](const float* base, std::size_t stride) {
        return _mm_setr_ps(base[0], base[stride], base[2 * stride], 0.0f);
    };
    V delayed[6];
    for (std::size_t line = 0; line < 6; ++line) delayed[line] = load_state(history_.delayed.data() + line, 6);
    V low_pass = load_state(history_.low_pass.data(), 1);
    V previous = load_state(history_.previous_input.data(), 1);
    // The constants are flushed once (the reference flushed them at every use).
    const V comb[3] = {vec::operand<M>(_mm_set1_ps(comb_[0])), vec::operand<M>(_mm_set1_ps(comb_[1])),
                       vec::operand<M>(_mm_set1_ps(comb_[2]))};
    const V color = vec::operand<M>(_mm_set1_ps(history_.parameters.color));
    const V damping = vec::operand<M>(_mm_set1_ps(damping_));
    const V wet = vec::operand<M>(_mm_set1_ps(wet_)), dry = vec::operand<M>(_mm_set1_ps(dry_));
    const V k03 = _mm_set1_ps(0.3f);
    const V sign = _mm_set1_ps(-0.0f);
    // One cursor step for line `line` of every channel: the cell's old value is the output, the input replaces it.
    const auto exchange = [&](std::size_t line, V input) noexcept {
        alignas(16) float in[4];
        _mm_store_ps(in, input);
        float out[3];
        for (std::size_t channel = 0; channel < kChannels; ++channel) {
            const std::size_t index = channel * 6 + line;
            float& cell = cell_base[index][position[index]];
            out[channel] = cell;
            cell = in[channel];
            if (++position[index] == kLength[index]) position[index] = 0;
        }
        return _mm_setr_ps(out[0], out[1], out[2], 0.0f);
    };
    // The flush mode's sample, speculatively: the same ops without the result flush of each one, which is what makes
    // the dependent chain long (a flush is a compare, an and and an andnot after every op). If no result of the chain
    // is suspect (zero, or above the smallest normal's magnitude: reverb_ops.h `suspect`), every flush would have been
    // the identity and the unflushed chain has the same bits; nothing has been written yet, so a sample with a
    // suspect result is simply run again through the exact code below. Reads of the delay cells do not depend on this
    // sample's stores (each line is its own region, read once and written once per sample).
    const auto peek = [&](std::size_t line) noexcept {
        return _mm_setr_ps(cell_base[line][position[line]], cell_base[6 + line][position[6 + line]],
                           cell_base[12 + line][position[12 + line]], 0.0f);
    };
    const auto store_cell = [&](std::size_t line, V value) noexcept {
        alignas(16) float in[4];
        _mm_store_ps(in, value);
        for (std::size_t channel = 0; channel < kChannels; ++channel) {
            const std::size_t index = channel * 6 + line;
            cell_base[index][position[index]] = in[channel];
            if (++position[index] == kLength[index]) position[index] = 0;
        }
    };
    // Values read from the state (a delay cell, `delayed`, `low_pass`, `previous`) may be subnormal when the history
    // was restored from outside: they are flushed where they are used, as the reference did. A value an op of this
    // loop produced is flush-fixed already (+-0, normal, inf, NaN): the *_clean ops take it as it is.
    for (std::size_t i = 0; i < frames; ++i) {
        const V input = _mm_setr_ps(float(channels[0][i]), float(channels[1][i]), float(channels[2][i]), 0.0f);
        if constexpr (M == kFlush) {
            V out[6], feedbacks[6];
            for (std::size_t line = 0; line < 6; ++line) out[line] = peek(line);
            const V din = pre_delay_ ? vec::operand<M>(previous) : input;
            for (std::size_t line = 0; line < 3; ++line)
                feedbacks[line] = vec::fused_clean<M>(comb[line], vec::operand<M>(delayed[line]), din);
            V bad;
            V signal = _mm_add_ps(vec::operand<M>(out[0]), vec::operand<M>(out[1]));
            bad = vec::suspect(signal);
            signal = _mm_add_ps(signal, vec::operand<M>(out[2]));
            bad = _mm_or_ps(bad, vec::suspect(signal));
            V low_pass_next = low_pass;
            for (std::size_t pass = 0; pass < 3; ++pass) {
                if (pass == 2) {
                    const V scaled = _mm_mul_ps(signal, k03);
                    low_pass_next = _mm_fmadd_ps(damping, vec::operand<M>(low_pass), scaled);
                    bad = _mm_or_ps(bad, _mm_or_ps(vec::suspect(scaled), vec::suspect(low_pass_next)));
                    signal = low_pass_next;
                }
                const V old = vec::operand<M>(delayed[3 + pass]);
                const V feedback = _mm_fmadd_ps(color, old, signal);
                const V swung = _mm_fmadd_ps(color, feedback, _mm_xor_ps(old, sign));
                bad = _mm_or_ps(bad, _mm_or_ps(vec::suspect(feedback), vec::suspect(swung)));
                signal = _mm_xor_ps(swung, sign);
                feedbacks[3 + pass] = feedback;
            }
            const V mixed = _mm_fmadd_ps(wet, signal, vec::mul_clean<M>(dry, input));
            bad = _mm_or_ps(bad, vec::suspect(mixed));
            if (_mm_movemask_ps(bad) == 0) {
                for (std::size_t line = 0; line < 6; ++line) {
                    store_cell(line, feedbacks[line]);
                    delayed[line] = out[line];
                }
                low_pass = low_pass_next;
                if (pre_delay_) previous = input;
                const __m128i result = vec::truncate<M>(mixed);
                channels[0][i] = _mm_cvtsi128_si32(result);
                channels[1][i] = _mm_extract_epi32(result, 1);
                channels[2][i] = _mm_extract_epi32(result, 2);
                continue;
            }
        }
        V delayed_input = input;
        if (pre_delay_) {
            delayed_input = vec::operand<M>(previous);
            previous = input;
        }
        V combined[3];
        for (std::size_t line = 0; line < 3; ++line) {
            const V feedback = vec::fused_clean<M>(comb[line], vec::operand<M>(delayed[line]), delayed_input);
            combined[line] = delayed[line] = exchange(line, feedback);
        }
        V signal = vec::add_clean<M>(
            vec::add_clean<M>(vec::operand<M>(combined[0]), vec::operand<M>(combined[1])),
            vec::operand<M>(combined[2]));
        for (std::size_t pass = 0; pass < 3; ++pass) {
            if (pass == 2) {
                signal = vec::fused_clean<M>(damping, vec::operand<M>(low_pass), vec::mul_clean<M>(signal, k03));
                low_pass = signal;
            }
            const std::size_t line = 3 + pass;
            const V old = vec::operand<M>(delayed[line]);
            const V feedback = vec::fused_clean<M>(color, old, signal);
            signal = _mm_xor_ps(vec::fused_clean<M>(color, feedback, _mm_xor_ps(old, sign)), sign);
            delayed[line] = exchange(line, feedback);
        }
        const __m128i result = vec::truncate<M>(vec::fused_clean<M>(wet, signal, vec::mul_clean<M>(dry, input)));
        channels[0][i] = _mm_cvtsi128_si32(result);
        channels[1][i] = _mm_extract_epi32(result, 1);
        channels[2][i] = _mm_extract_epi32(result, 2);
    }
    alignas(16) float lane[4];
    for (std::size_t line = 0; line < 6; ++line) {
        _mm_store_ps(lane, delayed[line]);
        for (std::size_t channel = 0; channel < kChannels; ++channel) history_.delayed[channel * 6 + line] = lane[channel];
    }
    _mm_store_ps(lane, low_pass);
    for (std::size_t channel = 0; channel < kChannels; ++channel) history_.low_pass[channel] = lane[channel];
    if (pre_delay_) {
        _mm_store_ps(lane, previous);
        for (std::size_t channel = 0; channel < kChannels; ++channel) history_.previous_input[channel] = lane[channel];
    }
    for (std::size_t index = 0; index < kLines; ++index)
        history_.positions[index] = static_cast<std::uint16_t>(position[index]);
}
#endif

void Reverb::process(const std::array<std::int32_t*, 3>& channels, std::size_t frames,
                     math::SubnormalMode mode) noexcept {
#if defined(__AVX2__)
    if (mode == kFlush) {
        if (cross_ != 0) crossfeed<kFlush>(channels, frames);
        run_vector<kFlush>(channels, frames);
    } else {
        if (cross_ != 0) crossfeed<Mode::Preserve>(channels, frames);
        run_vector<Mode::Preserve>(channels, frames);
    }
#else
    if (mode == kFlush)
        run<kFlush>(channels, frames);
    else
        run<Mode::Preserve>(channels, frames);
#endif
}

template <math::SubnormalMode M>
void Reverb::run(const std::array<std::int32_t*, 3>& channels, std::size_t frames) noexcept {
    if (cross_ != 0) crossfeed<M>(channels, frames);
    const float color = history_.parameters.color;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const std::size_t first = channel * 6;
        for (std::size_t i = 0; i < frames; ++i) {
            const float input = float(channels[channel][i]);
            float delayed_input = input;
            if (pre_delay_) {
                delayed_input = history_.previous_input[channel];
                history_.previous_input[channel] = input;
            }
            std::array<float, 3> combined{};
            for (std::size_t line = 0; line < 3; ++line) {
                const auto index = first + line;
                const float feedback = fused<M>(comb_[line], history_.delayed[index], delayed_input);
                combined[line] = history_.delayed[index] = exchange(index, feedback);
            }
            float signal = add<M>(add<M>(combined[0], combined[1]), combined[2]);
            for (std::size_t pass = 0; pass < 3; ++pass) {
                if (pass == 2) {
                    signal = fused<M>(damping_, history_.low_pass[channel], mul<M>(signal, 0.3f));
                    history_.low_pass[channel] = signal;
                }
                const auto index = first + 3 + pass;
                const float old = history_.delayed[index];
                const float feedback = fused<M>(color, old, signal);
                signal = -fused<M>(color, feedback, -old);
                history_.delayed[index] = exchange(index, feedback);
            }
            channels[channel][i] = truncate<M>(fused<M>(wet_, signal, mul<M>(dry_, input)));
        }
    }
}

} // namespace shadow::audio
