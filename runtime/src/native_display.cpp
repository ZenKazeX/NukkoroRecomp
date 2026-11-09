#include "native_display.h"

#include <cmath>
#include <cstring>

namespace shadow::display {

float coverage_scale(const gekko::DisplayLayout& layout) noexcept {
    const float scale = layout.coverage().affine_scale();
    return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}

void State::configure(const gekko::DisplayConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    width_ = config.window_width > 0 ? config.window_width : 640;
    height_ = config.window_height > 0 ? config.window_height : 480;
    publish_locked();
}

void State::resize(int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (width > 0) width_ = width;
    if (height > 0) height_ = height;
    publish_locked();
}

gekko::DisplayLayout State::layout() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return gekko::make_display_layout(config_, width_, height_);
}

void State::publish_locked() noexcept {
    const float scale = display::coverage_scale(
        gekko::make_display_layout(config_, width_, height_));
    std::uint32_t bits;
    static_assert(sizeof(bits) == sizeof(scale));
    std::memcpy(&bits, &scale, sizeof(bits));
    const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    std::uint32_t aspect_bits;
    std::memcpy(&aspect_bits, &aspect, sizeof(aspect_bits));
    aspect_.store(aspect_bits, std::memory_order_release);
    coverage_.store((std::uint64_t{1} << 32) | bits, std::memory_order_release);
}

void State::set_width_follows_height(bool on) noexcept {
    width_follows_height_.store(on, std::memory_order_release);
}

bool State::width_follows_height() const noexcept {
    return width_follows_height_.load(std::memory_order_acquire);
}

void State::set_game_framing(bool on) noexcept {
    game_framing_.store(on, std::memory_order_release);
}

bool State::game_framing() const noexcept {
    return game_framing_.load(std::memory_order_acquire);
}

float State::aspect() const noexcept {
    const std::uint32_t bits = aspect_.load(std::memory_order_acquire);
    float aspect;
    std::memcpy(&aspect, &bits, sizeof(aspect));
    return aspect;
}

float State::coverage_scale() const noexcept {
    const auto bits = static_cast<std::uint32_t>(coverage_.load(std::memory_order_acquire));
    float scale;
    std::memcpy(&scale, &bits, sizeof(scale));
    return scale;
}

bool State::configured() const noexcept {
    return (coverage_.load(std::memory_order_acquire) >> 32) != 0;
}

}  // namespace shadow::display
