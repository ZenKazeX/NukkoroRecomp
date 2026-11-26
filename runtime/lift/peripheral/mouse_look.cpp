#include "peripheral/mouse_look.h"

#include <algorithm>
#include <cmath>

namespace shadow::peripheral {

namespace {

constexpr int kRightDead = 25;
constexpr int kRightRange = 39;

// One axis: counts to the normalised input that rotates by the requested angle over one frame.
double rate_for(std::int64_t counts, double sensitivity, bool invert) noexcept {
    const double degrees = static_cast<double>(counts) * MouseLook::kDegreesPerCount * sensitivity;
    const double rate = degrees / (MouseLook::kFullRateDegreesPerSecond * MouseLook::kFrameSeconds);
    return invert ? -rate : rate;
}

// A camera stick axis in stick units, from a normalised rate. The inverse of the engine's axis reading: |value| - dead,
// over the range, with a negative deflection reading one unit closer to centre (manager_imp_pc.h).
std::int8_t stick_for_rate(double rate) noexcept {
    if (rate == 0.0) return 0;
    const double magnitude = std::min(std::abs(rate), 1.0);
    const int steps = std::max(1, static_cast<int>(std::lround(magnitude * kRightRange)));
    const int value = kRightDead + steps;
    return static_cast<std::int8_t>(rate > 0.0 ? value : -value - 1);
}

}  // namespace

void MouseLook::reset(std::int64_t total_x, std::int64_t total_y) noexcept {
    consumed_x_ = pending_x_ = total_x;
    consumed_y_ = pending_y_ = total_y;
    have_pending_x_ = have_pending_y_ = false;
}

MouseLook::Output MouseLook::peek(std::int64_t total_x, std::int64_t total_y) noexcept {
    Output output;
    const double yaw = rate_for(total_x - consumed_x_, settings_.sensitivity_x, settings_.invert_x);
    const double height = rate_for(total_y - consumed_y_, settings_.sensitivity_y, settings_.invert_y);
    have_pending_x_ = std::abs(yaw) >= kMinimumRate;
    have_pending_y_ = std::abs(height) >= kMinimumRate;
    pending_x_ = total_x;
    pending_y_ = total_y;
    if (have_pending_x_) output.yaw_rate = std::clamp(yaw, -1.0, 1.0);
    if (have_pending_y_) output.height_rate = std::clamp(height, -1.0, 1.0);
    return output;
}

void MouseLook::commit() noexcept {
    if (have_pending_x_) consumed_x_ = pending_x_;
    if (have_pending_y_) consumed_y_ = pending_y_;
    have_pending_x_ = have_pending_y_ = false;
}

void MouseCameraMap::update(std::int64_t mouse_x, std::int64_t mouse_y) noexcept {
    if (!started_) {
        started_ = true;
        last_x_ = mouse_x;
        last_y_ = mouse_y;
        return;
    }
    const std::int64_t dx = mouse_x - last_x_, dy = mouse_y - last_y_;
    last_x_ = mouse_x;
    last_y_ = mouse_y;
    // How far the mouse moved each way since the last call.
    const auto moved = [&](MouseMove move) -> std::int64_t {
        switch (move) {
            case MouseMove::Left: return dx < 0 ? -dx : 0;
            case MouseMove::Right: return dx > 0 ? dx : 0;
            case MouseMove::Up: return dy < 0 ? -dy : 0;
            case MouseMove::Down: return dy > 0 ? dy : 0;
            case MouseMove::None: return 0;
        }
        return 0;
    };
    // kCameraActions: left, right, up, down.
    stick_x_ += moved(map_[1]) - moved(map_[0]);
    stick_y_ += moved(map_[2]) - moved(map_[3]);
}

void apply_look_to_stick(ControllerSample& sample, const MouseLook::Output& look) noexcept {
    if (look.yaw_rate != 0.0) sample.substick_x = stick_for_rate(look.yaw_rate);
    if (look.height_rate != 0.0) sample.substick_y = stick_for_rate(look.height_rate);
}

}  // namespace shadow::peripheral
