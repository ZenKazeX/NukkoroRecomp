#pragma once

// The display settings while the game runs (the Options menu's Display tile): one owner of the window mode and size,
// the render size, the presentation cadence and the render-only extras, readable and changeable from any thread, and
// saved to display.cfg.
//
// The launcher resolves the settings at startup (the command line over display.cfg over the defaults) and hands them
// to initialize(); from then on apply() is the only way they change. apply() does not touch the window or the
// renderer itself: each group of settings is picked up by the thread that owns what it changes, at that thread's
// next pickup point (take_changes):
//
//   * kDisplayWindowChange  - the UI thread, once per loop iteration: the window mode and size, and the window's
//                             content aspect for an exact render size. The swap chain follows through the loop's
//                             existing client-size resize.
//   * kDisplayPresentChange - the UI thread: V-Sync (the next present), the render clock's rate and frame blending.
//   * kDisplayOverlayChange - the UI thread: the FPS counter.
//   * kDisplayGameChange    - the game thread, at the next host service (where a window resize is latched): the
//                             render size (the game-visible output when both sides are given) and the draw distance,
//                             which the game thread's driver reads.
//
// save() rewrites display.cfg in place: every byte of a line it does not own stays as it was (comments, blank lines,
// keys it does not manage, line endings); a managed key's line keeps its key spelling, spacing and trailing comment
// and only its value is replaced; keys the file lacks are appended. Only the keys apply() has changed since
// initialize() are written, so a value that came from the command line is never persisted unless the player changed
// that setting.

#include <filesystem>
#include <mutex>
#include <string>

namespace gekko {

struct DisplaySettings {
    bool fullscreen = false;  // borderless, covering the window's monitor
    int window_width = 1280;  // the windowed client size asked (the resolution)
    int window_height = 720;
    // The render size; 0 x 0 follows the window. Both given: exactly that size (docs/RESOLUTION_AUDIT.md).
    int render_width = 0;
    int render_height = 0;
    double render_rate = 60.0;  // presents per second; 0 = unlocked
    bool vsync = true;
    bool frame_interpolation = false;
    float draw_distance = 8.0f;  // 1 = the original game .. 8 (docs/DRAW_DISTANCE.md)
    bool fps_counter = false;
};

bool operator==(const DisplaySettings& a, const DisplaySettings& b) noexcept;
inline bool operator!=(const DisplaySettings& a, const DisplaySettings& b) noexcept { return !(a == b); }

// take_changes() groups (see the top of this file).
enum DisplaySettingsChange : unsigned {
    kDisplayWindowChange = 1u << 0,
    kDisplayPresentChange = 1u << 1,
    kDisplayOverlayChange = 1u << 2,
    kDisplayGameChange = 1u << 3,
};

class DisplaySettingsService {
public:
    // The launcher, once, before the game runs: the settings in effect and the display.cfg save() writes.
    void initialize(const DisplaySettings& settings, std::filesystem::path cfg_path);
    bool initialized() const;
    std::filesystem::path cfg_path() const;

    // Any thread: the settings as last applied.
    DisplaySettings current() const;
    // The ranges display.cfg and the command line accept: a window of at least 160 x 120, render sides 0..16384
    // (Direct3D 12's texture limit), a render rate of 0 (unlocked) or (0, 1000], a draw distance of 1..8.
    static bool validate(const DisplaySettings& settings, std::string* error);
    // Any thread: takes `settings` (false and `error` when validate() refuses it, nothing changes). The groups whose
    // values differ are marked for their owners' next take_changes().
    bool apply(const DisplaySettings& settings, std::string* error = nullptr);
    // Any thread: writes the keys changed since initialize() to display.cfg (see the top of this file). The file is
    // replaced whole (a temporary beside it, then renamed over it); false and `error` when it cannot be read or
    // written, and the file is left as it was.
    bool save(std::string* error = nullptr) const;

    // The owner of `groups`: which of them changed since its last call, and the settings now. Each group has one
    // owner; groups not asked for stay pending for theirs.
    unsigned take_changes(unsigned groups, DisplaySettings* settings);

private:
    mutable std::mutex mutex_;
    DisplaySettings current_;
    std::filesystem::path cfg_path_;
    bool initialized_ = false;
    unsigned pending_ = 0;
    unsigned changed_keys_ = 0;  // DisplaySettingsKey bits (display_settings.cpp) changed since initialize()
};

// The process's display settings: the launcher initializes it; the System's owners, the native menu owners and the
// UI thread reach it here.
DisplaySettingsService& display_settings();

}  // namespace gekko
