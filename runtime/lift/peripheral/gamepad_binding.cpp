#include "peripheral/gamepad_binding.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace shadow::peripheral {

namespace {

constexpr int kStickDeadZone = 8000;  // of 32767; just above the 7849 Windows recommends for a left stick

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

}  // namespace

std::int8_t stick_units(std::int16_t axis) noexcept {
    const int magnitude = axis < 0 ? -static_cast<int>(axis) : axis;
    if (magnitude < kStickDeadZone) return 0;
    const int scaled = (static_cast<int>(axis) * kStickFull) / 32767;
    return static_cast<std::int8_t>(std::clamp(scaled, -kStickFull, kStickFull));
}

bool parse_button_style(const std::string& text, ButtonStyle& style) {
    static constexpr struct {
        const char* name;
        ButtonStyle style;
    } kStyles[] = {{"auto", ButtonStyle::Auto},
                   {"original", ButtonStyle::Original},
                   {"xbox", ButtonStyle::Xbox},
                   {"playstation", ButtonStyle::PlayStation},
                   {"switch", ButtonStyle::Switch}};
    for (const auto& entry : kStyles) {
        if (text == entry.name) {
            style = entry.style;
            return true;
        }
    }
    return false;
}

const char* button_style_name(ButtonStyle style) noexcept {
    switch (style) {
        case ButtonStyle::Auto: return "auto";
        case ButtonStyle::Original: return "original";
        case ButtonStyle::Xbox: return "xbox";
        case ButtonStyle::PlayStation: return "playstation";
        case ButtonStyle::Switch: return "switch";
    }
    return "auto";
}

bool parse_gamepad_binding(const std::string& text, GamepadBinding& binding, std::string* error) {
    std::istringstream stream(text);
    std::string line;
    unsigned line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            if (line.find_first_not_of(" \t\r\n") != std::string::npos) {
                if (error) *error = "pad.cfg line " + std::to_string(line_number) + ": expected key=value";
                return false;
            }
            continue;
        }
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (key.empty()) continue;
        if (key == "button_style") {
            if (!parse_button_style(value, binding.button_style)) {
                if (error) {
                    *error = "pad.cfg line " + std::to_string(line_number) + ": '" + value +
                             "' is not a button style (auto, original, xbox, playstation, switch)";
                }
                return false;
            }
            continue;
        }
        unsigned long parsed = 0;
        try {
            // The whole value must be the number: "0x10zz" is an error, not 0x10.
            std::size_t used = 0;
            parsed = std::stoul(value, &used, 0);
            if (used != value.size()) throw std::invalid_argument(value);
        } catch (...) {
            if (error) {
                *error = "pad.cfg line " + std::to_string(line_number) + ": '" + value + "' is not a number";
            }
            return false;
        }
        // Range before narrowing: stoul accepts and wraps a leading '-', and a cast would turn
        // "trigger_threshold = 300" into 44 - a setting that loads and behaves like a different one.
        const bool is_threshold = key == "trigger_threshold";
        const unsigned long limit = is_threshold ? 0xFFul : 0xFFFFul;
        if (value.find('-') != std::string::npos || parsed > limit) {
            if (error) {
                *error = "pad.cfg line " + std::to_string(line_number) + ": '" + value +
                         "' is out of range for " + key + (is_threshold ? " (0 to 255)" : " (0 to 0xFFFF)");
            }
            return false;
        }
        const auto mask = static_cast<std::uint16_t>(parsed);
        if (key == "a") binding.a = mask;
        else if (key == "b") binding.b = mask;
        else if (key == "x") binding.x = mask;
        else if (key == "y") binding.y = mask;
        else if (key == "start") binding.start = mask;
        else if (key == "l") binding.l = mask;
        else if (key == "r") binding.r = mask;
        else if (key == "z") binding.z = mask;
        else if (key == "dpad_up") binding.up = mask;
        else if (key == "dpad_down") binding.down = mask;
        else if (key == "dpad_left") binding.left = mask;
        else if (key == "dpad_right") binding.right = mask;
        else if (key == "trigger_threshold") binding.trigger_threshold = static_cast<std::uint8_t>(parsed);
        else if (key == "swap_sticks") binding.swap_sticks = parsed != 0;
        else {
            if (error) *error = "pad.cfg line " + std::to_string(line_number) + ": unknown key '" + key + "'";
            return false;
        }
    }
    return true;
}

ControllerSample sample_from_gamepad(const GamepadState& state, const GamepadBinding& binding) noexcept {
    ControllerSample sample;
    sample.connected = true;
    const auto held = [&](std::uint16_t mask) { return mask != 0u && (state.buttons & mask) != 0u; };
    std::uint32_t controls = 0;
    if (held(binding.up)) controls |= kControlUp;
    if (held(binding.down)) controls |= kControlDown;
    if (held(binding.left)) controls |= kControlLeft;
    if (held(binding.right)) controls |= kControlRight;
    if (held(binding.start)) controls |= kControlStart;
    if (held(binding.l)) controls |= kControlL;
    if (held(binding.r)) controls |= kControlR;
    if (held(binding.z)) controls |= kControlZ;
    if (held(binding.a)) controls |= kControlA;
    if (held(binding.b)) controls |= kControlB;
    if (held(binding.x)) controls |= kControlX;
    if (held(binding.y)) controls |= kControlY;
    if (state.left_trigger > binding.trigger_threshold) controls |= kControlL;
    if (state.right_trigger > binding.trigger_threshold) controls |= kControlR;
    sample.controls = controls;
    const std::int16_t move_x = binding.swap_sticks ? state.right_x : state.left_x;
    const std::int16_t move_y = binding.swap_sticks ? state.right_y : state.left_y;
    const std::int16_t camera_x = binding.swap_sticks ? state.left_x : state.right_x;
    const std::int16_t camera_y = binding.swap_sticks ? state.left_y : state.right_y;
    sample.stick_x = stick_units(move_x);
    sample.stick_y = stick_units(move_y);
    sample.substick_x = stick_units(camera_x);
    sample.substick_y = stick_units(camera_y);
    return sample;
}

}  // namespace shadow::peripheral
