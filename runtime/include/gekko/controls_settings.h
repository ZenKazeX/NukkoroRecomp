#pragma once

// The player's keyboard and mouse controls while the game runs: controls.cfg beside options.cfg, read at startup,
// applied atomically, and re-read when the file changes (so rebinding takes effect without a restart). The gamepad's
// own binding stays in pad.cfg (peripheral/gamepad_binding.h).
//
// current() is a snapshot pointer, safe to take from the input thread every poll without a lock on the hot path's data:
// a change publishes a new immutable config. A file that fails to parse keeps every line it could read and reports the
// rest; a bad line never removes the other controls.

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "peripheral/action_map.h"

namespace gekko {

class ControlsSettings {
public:
    // Reads `path` (a missing file = the defaults). Problems go to `warnings`.
    void load(const std::filesystem::path& path, std::vector<std::string>* warnings = nullptr);
    // The config in effect. Never null.
    std::shared_ptr<const shadow::peripheral::ControlsConfig> current() const;
    // Applies a validated config now; false and `error` when validate_controls() refuses it.
    bool apply(const shadow::peripheral::ControlsConfig& config, std::string* error = nullptr);
    // Writes every setting to the file, keeping its comments and order; creates the file if there is none.
    bool save(std::string* error = nullptr);
    // Back to the default layout (applied, not saved).
    void restore_defaults();
    // Re-reads the file if it changed on disk since the last load or save. Returns true when the config changed.
    bool reload_if_changed(std::vector<std::string>* warnings = nullptr);
    std::filesystem::path path() const;
    // Incremented by every change: a reader caching derived data compares this.
    std::uint64_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }

private:
    void publish(const shadow::peripheral::ControlsConfig& config);

    mutable std::mutex mutex_;
    std::shared_ptr<const shadow::peripheral::ControlsConfig> current_ =
        std::make_shared<const shadow::peripheral::ControlsConfig>(shadow::peripheral::default_controls());
    std::filesystem::path path_;
    std::filesystem::file_time_type stamp_{};
    bool have_stamp_ = false;
    std::atomic<std::uint64_t> generation_{1};
};

ControlsSettings& controls_settings();

}  // namespace gekko
