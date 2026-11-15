#pragma once

// The player's keyboard and mouse as device-independent actions and axes.
//
// Three layers, each knowing only the one below it:
//
//   device levels    which keys and mouse buttons are down now (the host's raw input, gekko/raw_input.h)
//   action map       this file: named actions, each bound to up to kMaxBinds physical inputs (controls.cfg)
//   game controls    the controls the engine's peripheral manager reads (peripheral/controls.h)
//
// The game's player, camera and vehicle code still runs as the original game's logic and consumes those controls
// through the peripheral manager, so an action resolves to the control the game tests. An action is named for what
// it does in the game (docs/controls/ORIGINAL_GAMECUBE_CONTROL_MAP.md); it never decides gameplay itself: which of the
// game's behaviours a control triggers is still decided by the game's own state machines.
//
// Pure code: no window, no thread, no file. Config text is parsed here so the tests can run it; the settings service
// owns the file (gekko/controls_settings.h).

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "peripheral/controls.h"

namespace shadow::peripheral {

enum class Action : std::uint8_t {
    MoveForward,
    MoveBack,
    MoveLeft,
    MoveRight,
    Walk,  // holds the movement stick at the walk deflection
    CameraLeft,
    CameraRight,
    CameraUp,
    CameraDown,
    Jump,          // A: jump, jump dash, homing attack
    Attack,        // B: attack, use the held weapon, slide
    ContextAction, // X: spin dash, light dash, dark spin, pick up, enter a vehicle, throw
    Chaos,         // Y: Chaos Blast, Chaos Control, cast the weapon away
    ShoulderL,     // L: menus
    ParallelMove,  // R: held for parallel movement
    TriggerZ,      // Z: menus
    Pause,         // Start
    Back,          // Start during play and on the pause screen, B (back) in other menus
    MenuUp,      // d-pad
    MenuDown,
    MenuLeft,
    MenuRight,
    kCount,
};
inline constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::kCount);

// The config key of an action ("move_forward", "jump", ...), and back. action_from_name is exact (lower case).
const char* action_name(Action action) noexcept;
bool action_from_name(std::string_view name, Action& out) noexcept;
// The game control an action raises; 0 for the axis actions (movement, camera, walk).
std::uint32_t action_control(Action action) noexcept;

// Mouse buttons (bits of KeyMouseLevels::mouse_buttons, and Bind codes).
enum MouseButton : std::uint8_t {
    kMouseLeft = 0,
    kMouseRight = 1,
    kMouseMiddle = 2,
    kMouseX1 = 3,
    kMouseX2 = 4,
    kMouseButtonCount = 5,
};

// One physical input. Key codes are Windows virtual keys, with the left / right modifier codes distinct (VK_LSHIFT...).
struct Bind {
    enum class Source : std::uint8_t { None, Key, Mouse };
    Source source = Source::None;
    std::uint8_t code = 0;

    constexpr bool bound() const noexcept { return source != Source::None; }
    friend constexpr bool operator==(const Bind& a, const Bind& b) noexcept {
        return a.source == b.source && a.code == b.code;
    }
};
constexpr Bind key_bind(int virtual_key) noexcept {
    return Bind{Bind::Source::Key, static_cast<std::uint8_t>(virtual_key)};
}
constexpr Bind mouse_bind(MouseButton button) noexcept {
    return Bind{Bind::Source::Mouse, static_cast<std::uint8_t>(button)};
}

// "key:Space", "key:W", "mouse:Left"; a bare key name means key:. False for anything else.
bool parse_bind(std::string_view text, Bind& out) noexcept;
std::string format_bind(const Bind& bind);
// The name of a virtual key in config files ("Space", "W", "LShift", "F1", "0x7C" for one without a name).
std::string key_name(int virtual_key);
bool key_from_name(std::string_view name, int& virtual_key) noexcept;

inline constexpr std::size_t kMaxBinds = 3;
using ActionBinds = std::array<Bind, kMaxBinds>;

// What the player can configure.
struct ControlsConfig {
    std::array<ActionBinds, kActionCount> binds{};

    // Mouse camera. Sensitivity is degrees of camera yaw per mouse count at 1.0 before the scale below; height uses the
    // same scale in the game's height input units (docs/controls/CAMERA_CONTROL_RECONSTRUCTION.md).
    double mouse_sensitivity_x = 1.0;
    double mouse_sensitivity_y = 1.0;
    bool mouse_invert_x = false;
    bool mouse_invert_y = false;
    bool mouse_capture = true;  // hide and confine the cursor during gameplay

    // The stick units the Walk action holds the movement stick at (the walk band of the game's stick tuning).
    int walk_deflection = kDefaultWalkDeflection;

    static constexpr int kDefaultWalkDeflection = 36;

    friend bool operator==(const ControlsConfig&, const ControlsConfig&) = default;
};

// The default PC layout (docs/controls/KEYBOARD_MOUSE_BINDINGS.md).
ControlsConfig default_controls() noexcept;

// Parses controls.cfg text over `config`. Keys: one per action (action_name) with a comma-separated list of binds or
// "none", and mouse_sensitivity_x/y, mouse_invert_x/y, mouse_capture, walk_deflection. A malformed line is an error and
// is not applied (`errors` gets one message per problem, naming the line); the rest of the file still applies, so one
// typo cannot take every control away. Returns false when anything was reported.
bool parse_controls(const std::string& text, ControlsConfig& config, std::vector<std::string>* errors);

// Validates ranges; false and `error` when the config cannot be used.
bool validate_controls(const ControlsConfig& config, std::string* error);

// The (key, value) of every setting that differs from `base`, in file order. The settings service writes these.
struct ControlsSetting {
    std::string key, value;
};
std::vector<ControlsSetting> controls_settings(const ControlsConfig& config);

// Actions bound to `bind`, excluding `ignore` (Action::kCount for none): what a rebinding UI reports as a conflict.
std::vector<Action> bound_actions(const ControlsConfig& config, Bind bind, Action ignore = Action::kCount);

// ---- resolving ---------------------------------------------------------------------------------------------------

// The physical levels the host read at one moment.
struct KeyMouseLevels {
    std::bitset<256> keys;
    std::uint8_t mouse_buttons = 0;  // bit = MouseButton

    bool down(const Bind& bind) const noexcept {
        switch (bind.source) {
            case Bind::Source::Key: return keys.test(bind.code);
            case Bind::Source::Mouse: return bind.code < kMouseButtonCount && ((mouse_buttons >> bind.code) & 1u) != 0u;
            case Bind::Source::None: return false;
        }
        return false;
    }
};

// The per-action state one read of the devices produces.
struct ActionState {
    std::array<bool, kActionCount> held{};
    bool any_kbm_input = false;  // any bound key or mouse button is down

    bool down(Action action) const noexcept { return held[static_cast<std::size_t>(action)]; }
};

// `in_menu`: the game is showing a menu rather than play. Mouse buttons then mean nothing, so a stray click cannot
// confirm or back out of a screen.
ActionState resolve_actions(const ControlsConfig& config, const KeyMouseLevels& levels, bool in_menu = false) noexcept;

// The actions as the sample the game reads: controls held and the movement stick. Keys are all-or-nothing, so a
// direction is full deflection, or the walk deflection while Walk is held; opposite keys cancel. A diagonal uses the
// raw value the engine renormalises to exactly length 1.0 (docs/controls/PLAYER_MOVEMENT_INPUT.md section 7), so
// diagonal and straight running reach the same speed.
// `back_means_b`: a menu other than the pause screen, where Back is the game's cancel button.
ControllerSample sample_from_actions(const ControlsConfig& config, const ActionState& actions,
                                     bool back_means_b = false) noexcept;

}  // namespace shadow::peripheral
