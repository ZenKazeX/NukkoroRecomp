#pragma once

// The display settings while the game runs (the Options menu's Display and Graphics pages): one owner of the window
// mode and sizes, the game resolution, the render scale, the presentation cadence and the render-only extras,
// readable and changeable from any thread, and saved to display.cfg (docs/DISPLAY_SETTINGS_AUDIT.md).
//
// Three layers, never mixed:
//   * preferences - display.cfg, the only persistent store. save() writes only CONFIRMED values, and only the keys
//                   the player changed this session (a key changed and changed back by a rollback is not written);
//   * session overrides - the command line (tests, benchmarks): in effect for this run, never saved unless the
//                   player changes that setting in the menu (then the override for it ends);
//   * the achieved state - what the window owner actually reached (exclusive mode set or refused, the client size,
//                   the monitor). The menu and the session report show it next to the settings.
//
// Changes are transactions: apply() makes a value current, either committed at once (safe changes) or provisional
// (a window mode or screen resolution change, which may show nothing on this monitor): a provisional change is
// committed by commit() - the player's confirmation, or the window owner's verified success for Alt+Enter - or put
// back by rollback(). While one is provisional, save() writes the last committed values only.
//
// Each group of settings is picked up by the thread that owns what it changes, at that thread's next pickup point
// (take_changes):
//   * kDisplayWindowChange  - the UI thread: the window mode, the window size and the exclusive mode.
//   * kDisplayPresentChange - the UI thread: V-Sync, the render clock's rate, frame blending, MSAA, filtering.
//   * kDisplayOverlayChange - the UI thread: the FPS counter.
//   * kDisplayGameChange    - the game thread, at the next host service: the screen ratio, the resolution scale,
//                             the draw distance and the gameplay aspect.
//
// save() rewrites display.cfg in place: every byte of a line it does not own stays as it was (comments, blank lines,
// keys it does not manage, line endings); a managed key's line keeps its key spelling, spacing and trailing comment
// and only its value is replaced; keys the file lacks are appended.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace gekko {

struct DisplaySettings {
    // Display mode (the menu: Windowed / Fullscreen). Fullscreen is borderless at the desktop's size; exclusive is a
    // display.cfg / command-line key for tests only (the monitor's mode fullscreen_width x fullscreen_height, 0 x 0 =
    // its desktop mode), never offered in the menu.
    bool fullscreen = false;
    bool exclusive = false;
    // The window's client size: the Resolution row's choice or whatever the player dragged it to, remembered.
    int window_width = 1280;
    int window_height = 720;
    int fullscreen_width = 0;
    int fullscreen_height = 0;
    double aspect = 0.0;          // the screen ratio: 0 = Auto (the window's / screen's own shape)
    double render_scale = 100.0;  // the resolution scale: percent per side of the picture (25..200)
    // Image quality, independent of the above: the scene's MSAA sample count (1 = single-sample; 2, 4, 8, ... as the
    // GPU supports - the renderer refuses and reports one it does not, the setting stays as asked) and the
    // anisotropic filtering level of the game's filtered textures (1 = Off: the console's linear filter; 2..16).
    int msaa = 4;
    int anisotropy = 16;
    // The aspect the game LOGIC's on-screen tests use (docs/RESOLUTION_AUDIT.md section 13): 0 = follow the picture
    // (the default - live play as always); 4/3 = the console's original; any ratio. Replays take the recorded one.
    double gameplay_aspect = 0.0;
    double render_rate = 60.0;  // presents per second; 0 = unlocked
    bool vsync = true;
    bool frame_interpolation = false;
    float draw_distance = 1.0f;  // 1 = the original game (the default) .. 8 (docs/DRAW_DISTANCE.md)
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

// What the window owner reached for the settings of `generation` (UI thread, after it applied them).
struct DisplayWindowState {
    std::uint64_t generation = 0;  // the settings generation this state answers (0: none reported yet)
    bool fullscreen = false;
    bool exclusive_wanted = false;
    bool exclusive_active = false;  // the monitor is in the asked mode now
    int client_width = 0, client_height = 0;
    int monitor_width = 0, monitor_height = 0;
    std::string note;               // the window owner's explanation (a refused mode), empty when all is as asked
};

class DisplaySettingsService {
public:
    // The launcher, once, before the game runs: the settings in effect (preferences + session overrides), the keys
    // the command line set (session overrides, display_settings.cpp key names) and the display.cfg save() writes.
    void initialize(const DisplaySettings& settings, std::filesystem::path cfg_path, unsigned override_keys = 0u);
    bool initialized() const;
    std::filesystem::path cfg_path() const;

    // Any thread: the settings as last applied (provisional ones included) / as last committed.
    DisplaySettings current() const;
    DisplaySettings confirmed() const;
    bool provisional() const;
    // Changes with every apply / commit / rollback: owners of a snapshot (the Options page) re-read when it moved.
    std::uint64_t generation() const;
    // The ranges display.cfg and the command line accept (gekko/display_geometry.h): a window and an exclusive mode
    // of at least 320 x 240 (0 x 0 = the desktop's mode), a resolution scale of 25..200 %, an aspect of 0 (Auto) or
    // 0.25..8, a render rate of 0 (unlocked) or (0, 1000], a draw distance of 1..8.
    static bool validate(const DisplaySettings& settings, std::string* error);
    // Any thread: takes `settings` (false and `error` when validate() refuses it, nothing changes). Committed at once
    // unless `provisional`. The groups whose values differ are marked for their owners' next take_changes().
    bool apply(const DisplaySettings& settings, std::string* error = nullptr, bool provisional = false);
    void commit();    // the provisional values become the committed ones
    void rollback();  // the committed values become current again (their groups are marked)
    // Any thread: writes the committed keys the player changed this session to display.cfg (see the top of this
    // file). The file is replaced whole (a temporary beside it, then renamed over it); false and `error` when it
    // cannot be read or written, and the file is left as it was.
    bool save(std::string* error = nullptr) const;

    // The command line's settings in effect this session (display_settings.cpp key names, comma separated; empty:
    // none). A setting the player changes stops being overridden.
    std::string session_overrides() const;
    bool overridden(const char* key_name) const;

    // The window owner's achieved state (UI thread) and its readers.
    void report_window(const DisplayWindowState& state);
    DisplayWindowState window_state() const;

    // The owner of `groups`: which of them changed since its last call, and the settings now. Each group has one
    // owner; groups not asked for stay pending for theirs.
    unsigned take_changes(unsigned groups, DisplaySettings* settings);

private:
    mutable std::mutex mutex_;
    DisplaySettings current_;
    DisplaySettings confirmed_;
    DisplaySettings initial_;
    std::filesystem::path cfg_path_;
    bool initialized_ = false;
    bool provisional_ = false;
    std::uint64_t generation_ = 1;
    unsigned pending_ = 0;
    unsigned touched_keys_ = 0;    // keys the player committed a change of this session (written by save)
    unsigned override_keys_ = 0;   // keys the command line set and the player has not changed
    DisplayWindowState window_;
};

// display.cfg's key bit for a key name (display_settings.cpp), 0 when it names none: the launcher marks the keys the
// command line set with it.
unsigned display_settings_key(const char* name) noexcept;

// The process's display settings: the launcher initializes it; the System's owners, the native menu owners and the
// UI thread reach it here.
DisplaySettingsService& display_settings();

}  // namespace gekko
