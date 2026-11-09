#pragma once

// The logic of the button mapping configurator (ShadowPadConfig.exe): pad.cfg as the game reads it, without any window.
// The format is defined by the game's own parser (peripheral/gamepad_binding.h), which this program links.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

#include "peripheral/gamepad_binding.h"

namespace padconfig {

using shadow::peripheral::GamepadBinding;

// One button of the game's controller.
struct Control {
    const char* key;   // its pad.cfg key
    const char* name;  // as shown
    std::uint16_t GamepadBinding::*field;
};
inline constexpr std::array<Control, 12> kControls = {{
    {"a", "A", &GamepadBinding::a},
    {"b", "B", &GamepadBinding::b},
    {"x", "X", &GamepadBinding::x},
    {"y", "Y", &GamepadBinding::y},
    {"start", "Start", &GamepadBinding::start},
    {"l", "L", &GamepadBinding::l},
    {"r", "R", &GamepadBinding::r},
    {"z", "Z", &GamepadBinding::z},
    {"dpad_up", "D-Pad Up", &GamepadBinding::up},
    {"dpad_down", "D-Pad Down", &GamepadBinding::down},
    {"dpad_left", "D-Pad Left", &GamepadBinding::left},
    {"dpad_right", "D-Pad Right", &GamepadBinding::right},
}};

// What a binding of `mask` reads as: "A", "A + B", or "Not set".
std::string button_names(std::uint16_t mask);

// The first button held in `now` that was not held in `before`; 0 when none. Buttons already down when a capture
// starts do not count.
std::uint16_t newly_pressed(std::uint16_t before, std::uint16_t now) noexcept;

// Puts `mask` on control `index`. A button can do one job: a control already using it takes the old binding of
// `index` when that was all it had, so a swap never leaves a control empty.
void assign(GamepadBinding& binding, std::size_t index, std::uint16_t mask) noexcept;

bool same(const GamepadBinding& a, const GamepadBinding& b) noexcept;

// The text of pad.cfg for a binding.
std::string format_binding(const GamepadBinding& binding);

enum class LoadResult { Loaded, Absent, Invalid };
// `binding` starts at the defaults. Invalid: the file exists but the game would refuse it (`error` says why); the
// defaults are returned and the file is left alone.
LoadResult load_binding(const std::filesystem::path& path, GamepadBinding& binding, std::string* error);

// Writes beside the target and renames over it, so a failed save leaves the old file whole. Refuses anything the
// game's parser would not read back as exactly this binding.
bool save_binding(const std::filesystem::path& path, const GamepadBinding& binding, std::string* error);

}  // namespace padconfig
