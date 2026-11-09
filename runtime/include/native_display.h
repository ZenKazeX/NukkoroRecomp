#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "gekko/display.h"

namespace shadow::display {

float coverage_scale(const gekko::DisplayLayout& layout) noexcept;

// Process-owned presentation policy, independent of any graphics/device model.
// Configure before starting consumers. Resize may run on the window thread;
// layout reads are coherent snapshots and camera reads never take its mutex.
// The owner must outlive all consumers, including the reference rasterizer.
class State final {
public:
    void configure(const gekko::DisplayConfig& config);
    void resize(int width, int height);
    gekko::DisplayLayout layout() const;
    float coverage_scale() const noexcept;
    bool configured() const noexcept;
    // The native renderer (the lifted build) draws each view's picture with render/view.h displayed_window() - the
    // authored picture fitted to the view - instead of widening the game's horizontal view as the legacy renderer
    // did (coverage_scale). Camera consumers that must agree with the picture (world visibility) take the same
    // function. Set before the game runs.
    void set_width_follows_height(bool on) noexcept;
    bool width_follows_height() const noexcept;
    // The authored picture's framing there (render/view.h Framing): the game camera's own horizontal extent instead
    // of square pixels. Set before the game runs.
    void set_game_framing(bool on) noexcept;
    bool game_framing() const noexcept;
    // The presented output's width / height (the window's client area; the configured size until there is one).
    float aspect() const noexcept;

private:
    void publish_locked() noexcept;
    mutable std::mutex mutex_;
    gekko::DisplayConfig config_{};
    int width_ = 640;
    int height_ = 480;
    // One publication carries both readiness and the binary32 scale. A default
    // 1.0 before configuration must remain distinguishable from real 4:3.
    std::atomic<std::uint64_t> coverage_{0x3f800000u};
    std::atomic<std::uint32_t> aspect_{0x3faaaaabu};  // binary32 width / height, 4:3 until configured
    std::atomic<bool> width_follows_height_{false};
    std::atomic<bool> game_framing_{false};
};

}  // namespace shadow::display
