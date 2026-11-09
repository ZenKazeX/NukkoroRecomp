#include "audio/reverb.h"

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

void Reverb::process(const std::array<std::int32_t*, 3>& channels, std::size_t frames,
                     math::SubnormalMode mode) noexcept {
    if (mode == kFlush)
        run<kFlush>(channels, frames);
    else
        run<Mode::Preserve>(channels, frames);
}

template <math::SubnormalMode M>
void Reverb::run(const std::array<std::int32_t*, 3>& channels, std::size_t frames) noexcept {
    if (cross_ != 0) for (std::size_t i = 0; i < frames; ++i) {
        const float left = float(channels[0][i]), right = float(channels[1][i]);
        // Paired crossfeed uses separately rounded products, not a fused sum.
        channels[0][i] = nearest<M>(add<M>(mul<M>(left, direct_), mul<M>(right, cross_)));
        channels[1][i] = nearest<M>(add<M>(mul<M>(left, cross_), mul<M>(right, direct_)));
    }
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
