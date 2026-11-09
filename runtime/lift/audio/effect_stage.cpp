#include "audio/effect_stage.h"

#include <limits>

namespace shadow::audio {
namespace {
bool valid(EffectStage::Mode mode) noexcept {
    return mode == EffectStage::Mode::Direct || mode == EffectStage::Mode::Stereo ||
           mode == EffectStage::Mode::Opposite || mode == EffectStage::Mode::Matrix;
}
bool valid(EffectStage::Control control) noexcept { return valid(control.mode); }

std::int32_t add(std::int32_t left, std::int32_t right) noexcept {
    const auto sum = std::int64_t(left) + right;
    constexpr auto lo = std::numeric_limits<std::int32_t>::min();
    constexpr auto hi = std::numeric_limits<std::int32_t>::max();
    return sum < lo ? lo : sum > hi ? hi : static_cast<std::int32_t>(sum);
}
} // namespace

bool EffectStage::reset(const Reverb::Parameters& parameters, Control control) noexcept {
    if (!valid(control) || !effect_.configure(parameters)) return false;
    output_.reset(); pending_ = {}; spatial_right_ = {}; next_ = 0; awaiting_ = false;
    scheduled_ = live_ = control;
    return true;
}

bool EffectStage::configure(const Reverb::Parameters& parameters) noexcept {
    return effect_.configure(parameters);
}

bool EffectStage::control(Control next) noexcept {
    if (!valid(next)) return false;
    live_ = next;
    return true;
}

EffectStage::History EffectStage::history() const noexcept {
    return {effect_.history(), output_.history(), pending_, spatial_right_, next_, scheduled_, live_, awaiting_};
}

bool EffectStage::restore(const History& saved) noexcept {
    if (saved.next >= pending_.size() || !valid(saved.scheduled) || !valid(saved.live) ||
        !effect_.restore(saved.effect)) return false;
    output_.restore(saved.output); pending_ = saved.pending; spatial_right_ = saved.spatial_right;
    next_ = saved.next; scheduled_ = saved.scheduled; live_ = saved.live; awaiting_ = saved.awaiting;
    return true;
}

bool EffectStage::begin(Mode mode, Buffer& left, Buffer& right, Buffer& surround) const noexcept {
    if (!valid(mode)) return false;
    output_.begin(mode, left, right, surround);
    return true;
}

bool EffectStage::finish(Control scheduled, Buffer& left, Buffer& right, Buffer& surround, const Channels& room,
                         const Channels& spatial) noexcept {
    if (!valid(scheduled)) return false;
    if (awaiting_) advance(); // the preceding block's callback never ran: its tail stays frozen
    auto matrix_left = spatial[0], matrix_right = spatial[1];
    if (scheduled.enabled) {
        pending_[next_] = room;
        if (scheduled.mode == Mode::Matrix) spatial_right_[next_] = spatial[2];
        const auto returned = (next_ + 1) % pending_.size();
        for (unsigned i = 0; i < OutputStage::kSamples; ++i) {
            left[i] = add(left[i], pending_[returned][0][i]);
            right[i] = add(right[i], pending_[returned][1][i]);
            if (scheduled.mode == Mode::Matrix) {
                matrix_left[i] = add(matrix_left[i], pending_[returned][2][i]);
                matrix_right[i] = add(matrix_right[i], spatial_right_[returned][i]);
            } else surround[i] = add(surround[i], pending_[returned][2][i]);
        }
    }
    output_.finish(scheduled.mode, left, right, surround, matrix_left, matrix_right);
    awaiting_ = true;
    return true;
}

bool EffectStage::process(math::SubnormalMode mode) noexcept {
    if (!awaiting_) return false;
    auto& pending = pending_[(next_ + 2) % pending_.size()];
    effect_.process({pending[0].data(), pending[1].data(), pending[2].data()}, OutputStage::kSamples, mode);
    advance();
    return true;
}

void EffectStage::advance() noexcept {
    if (++next_ == pending_.size()) next_ = 0;
    awaiting_ = false;
}

EffectStage::Channels EffectStage::render(const Input& input, math::SubnormalMode mode) noexcept {
    Channels result{};
    begin(scheduled_.mode, result[0], result[1], result[2]);
    for (unsigned channel = 0; channel < result.size(); ++channel)
        for (unsigned i = 0; i < OutputStage::kSamples; ++i)
            result[channel][i] = add(result[channel][i], input.main[channel][i]);
    finish(scheduled_, result[0], result[1], result[2], input.room, input.spatial);
    if (live_.enabled) process(mode);
    else advance();
    scheduled_ = live_;
    return result;
}

} // namespace shadow::audio
