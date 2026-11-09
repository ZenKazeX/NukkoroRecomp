#include "audio/output_stage.h"

#include <limits>

namespace shadow::audio {
namespace {
std::int32_t limit(std::int64_t value) noexcept {
    constexpr auto lo = std::numeric_limits<std::int32_t>::min();
    constexpr auto hi = std::numeric_limits<std::int32_t>::max();
    return value < lo ? lo : value > hi ? hi : static_cast<std::int32_t>(value);
}
} // namespace

void OutputStage::begin(Mode mode, Buffer& left, Buffer& right, Buffer& surround) const noexcept {
    surround.fill(0);
    if (mode == Mode::Stereo || mode == Mode::Opposite) {
        right = history_.surround;
        if (mode == Mode::Stereo) left = right;
        else for (unsigned i = 0; i < kSamples; ++i) left[i] = limit(-std::int64_t(right[i]));
    } else { left.fill(0); right.fill(0); }
}

void OutputStage::finish(Mode mode, Buffer& left, Buffer& right, const Buffer& surround,
                         const Buffer& matrix_left, const Buffer& matrix_right) noexcept {
    if (mode == Mode::Matrix) {
        const auto& returned = history_.alternate_return ? history_.alternate_matrix : history_.matrix;
        auto& next = history_.alternate_return ? history_.matrix : history_.alternate_matrix;
        for (unsigned i = 0; i < kSamples; ++i) {
            left[i] = limit(std::int64_t(left[i]) + returned[0][i]);
            right[i] = limit(std::int64_t(right[i]) - returned[1][i]);
        }
        next[0] = matrix_left;
        next[1] = matrix_right;
    }
    history_.alternate_return = !history_.alternate_return;
    history_.surround = surround;
}

} // namespace shadow::audio
