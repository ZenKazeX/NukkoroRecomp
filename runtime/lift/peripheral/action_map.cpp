#include "peripheral/action_map.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>

namespace shadow::peripheral {

namespace {

// Windows virtual-key codes (stable values; WinUser.h).
constexpr int kVkBack = 0x08, kVkTab = 0x09, kVkReturn = 0x0D, kVkPause = 0x13, kVkCaps = 0x14, kVkEscape = 0x1B,
              kVkSpace = 0x20, kVkPrior = 0x21, kVkNext = 0x22, kVkEnd = 0x23, kVkHome = 0x24, kVkLeft = 0x25,
              kVkUp = 0x26, kVkRight = 0x27, kVkDown = 0x28, kVkInsert = 0x2D, kVkDelete = 0x2E, kVkF1 = 0x70,
              kVkNumpad0 = 0x60, kVkLShift = 0xA0, kVkRShift = 0xA1, kVkLCtrl = 0xA2, kVkRCtrl = 0xA3, kVkLAlt = 0xA4,
              kVkRAlt = 0xA5;

struct ActionRow {
    const char* name;
    std::uint32_t control;
};
constexpr std::array<ActionRow, kActionCount> kActions{{
    {"move_forward", 0},
    {"move_back", 0},
    {"move_left", 0},
    {"move_right", 0},
    {"walk", 0},
    {"camera_left", 0},
    {"camera_right", 0},
    {"camera_up", 0},
    {"camera_down", 0},
    {"jump", kControlA},
    {"attack", kControlB},
    {"action", kControlX},
    {"chaos", kControlY},
    {"shoulder_l", kControlL},
    {"parallel_move", kControlR},
    {"trigger_z", kControlZ},
    {"pause", kControlStart},
    {"back", 0},
    {"menu_up", kControlUp},
    {"menu_down", kControlDown},
    {"menu_left", kControlLeft},
    {"menu_right", kControlRight},
}};

struct KeyName {
    const char* name;
    int code;
};
constexpr KeyName kKeyNames[] = {
    {"Backspace", kVkBack}, {"Tab", kVkTab},       {"Enter", kVkReturn},  {"Pause", kVkPause},   {"CapsLock", kVkCaps},
    {"Esc", kVkEscape},     {"Space", kVkSpace},   {"PageUp", kVkPrior},  {"PageDown", kVkNext}, {"End", kVkEnd},
    {"Home", kVkHome},      {"Left", kVkLeft},     {"Up", kVkUp},         {"Right", kVkRight},   {"Down", kVkDown},
    {"Insert", kVkInsert},  {"Delete", kVkDelete}, {"LShift", kVkLShift}, {"RShift", kVkRShift}, {"LCtrl", kVkLCtrl},
    {"RCtrl", kVkRCtrl},    {"LAlt", kVkLAlt},     {"RAlt", kVkRAlt},
};

constexpr const char* kMouseNames[kMouseButtonCount] = {"Left", "Right", "Middle", "X1", "X2"};

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                                              [](char x, char y) { return lower(x) == lower(y); });
}

bool parse_bool(std::string_view text, bool& out) noexcept {
    if (text == "1" || iequals(text, "true") || iequals(text, "yes") || iequals(text, "on")) {
        out = true;
        return true;
    }
    if (text == "0" || iequals(text, "false") || iequals(text, "no") || iequals(text, "off")) {
        out = false;
        return true;
    }
    return false;
}

template <typename T>
bool parse_number(std::string_view text, T& out) noexcept {
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

constexpr double kSensitivityMin = 0.01, kSensitivityMax = 50.0;

constexpr int kLeftStickDead = 15;
constexpr int kFullDiagonal = 75;

}  // namespace

const char* action_name(Action action) noexcept {
    const auto index = static_cast<std::size_t>(action);
    return index < kActionCount ? kActions[index].name : "";
}

bool action_from_name(std::string_view name, Action& out) noexcept {
    for (std::size_t index = 0; index < kActionCount; ++index) {
        if (name == kActions[index].name) {
            out = static_cast<Action>(index);
            return true;
        }
    }
    return false;
}

std::uint32_t action_control(Action action) noexcept {
    const auto index = static_cast<std::size_t>(action);
    return index < kActionCount ? kActions[index].control : 0u;
}

std::string key_name(int virtual_key) {
    if ((virtual_key >= '0' && virtual_key <= '9') || (virtual_key >= 'A' && virtual_key <= 'Z')) {
        return std::string(1, static_cast<char>(virtual_key));
    }
    if (virtual_key >= kVkF1 && virtual_key < kVkF1 + 24) return "F" + std::to_string(virtual_key - kVkF1 + 1);
    if (virtual_key >= kVkNumpad0 && virtual_key < kVkNumpad0 + 10) {
        return "Num" + std::to_string(virtual_key - kVkNumpad0);
    }
    for (const KeyName& row : kKeyNames) {
        if (row.code == virtual_key) return row.name;
    }
    char text[8];
    std::snprintf(text, sizeof text, "0x%02X", static_cast<unsigned>(virtual_key) & 0xFFu);
    return text;
}

bool key_from_name(std::string_view name, int& virtual_key) noexcept {
    if (name.size() == 1) {
        const char c = name[0];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) {
            virtual_key = c;
            return true;
        }
        if (c >= 'a' && c <= 'z') {
            virtual_key = c - 'a' + 'A';
            return true;
        }
    }
    if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f')) {
        int number = 0;
        if (parse_number(name.substr(1), number) && number >= 1 && number <= 24) {
            virtual_key = kVkF1 + number - 1;
            return true;
        }
    }
    if (name.size() == 4 && iequals(name.substr(0, 3), "num") && name[3] >= '0' && name[3] <= '9') {
        virtual_key = kVkNumpad0 + (name[3] - '0');
        return true;
    }
    for (const KeyName& row : kKeyNames) {
        if (iequals(name, row.name)) {
            virtual_key = row.code;
            return true;
        }
    }
    if (name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        unsigned value = 0;
        const char* begin = name.data() + 2;
        const char* end = name.data() + name.size();
        const auto result = std::from_chars(begin, end, value, 16);
        if (result.ec == std::errc{} && result.ptr == end && value > 0u && value < 0xFFu) {
            virtual_key = static_cast<int>(value);
            return true;
        }
    }
    return false;
}

bool parse_bind(std::string_view text, Bind& out) noexcept {
    text = trim(text);
    constexpr std::string_view kKey = "key:", kMouse = "mouse:";
    if (text.size() > kMouse.size() && iequals(text.substr(0, kMouse.size()), kMouse)) {
        const std::string_view name = text.substr(kMouse.size());
        for (std::uint8_t button = 0; button < kMouseButtonCount; ++button) {
            if (iequals(name, kMouseNames[button])) {
                out = mouse_bind(static_cast<MouseButton>(button));
                return true;
            }
        }
        return false;
    }
    if (text.size() > kKey.size() && iequals(text.substr(0, kKey.size()), kKey)) text.remove_prefix(kKey.size());
    int key = 0;
    if (text.empty() || !key_from_name(text, key)) return false;
    out = key_bind(key);
    return true;
}

std::string format_bind(const Bind& bind) {
    switch (bind.source) {
        case Bind::Source::Key: return "key:" + key_name(bind.code);
        case Bind::Source::Mouse:
            return std::string("mouse:") + (bind.code < kMouseButtonCount ? kMouseNames[bind.code] : "Left");
        case Bind::Source::None: break;
    }
    return "none";
}

ControlsConfig default_controls() noexcept {
    ControlsConfig config;
    const auto set = [&](Action action, std::initializer_list<Bind> binds) {
        ActionBinds& row = config.binds[static_cast<std::size_t>(action)];
        std::size_t slot = 0;
        for (const Bind& bind : binds) row[slot++] = bind;
    };
    set(Action::MoveForward, {key_bind('W')});
    set(Action::MoveBack, {key_bind('S')});
    set(Action::MoveLeft, {key_bind('A')});
    set(Action::MoveRight, {key_bind('D')});
    set(Action::Walk, {key_bind(kVkLCtrl)});
    set(Action::Jump, {key_bind(kVkSpace)});
    set(Action::Attack, {mouse_bind(kMouseLeft)});
    set(Action::ContextAction, {key_bind('E')});
    set(Action::Chaos, {key_bind('F'), mouse_bind(kMouseRight)});
    set(Action::ShoulderL, {key_bind('Q')});
    set(Action::ParallelMove, {key_bind(kVkLShift)});
    set(Action::TriggerZ, {key_bind('C')});
    set(Action::Pause, {key_bind(kVkReturn)});
    set(Action::Back, {key_bind(kVkEscape)});
    set(Action::MenuUp, {key_bind(kVkUp)});
    set(Action::MenuDown, {key_bind(kVkDown)});
    set(Action::MenuLeft, {key_bind(kVkLeft)});
    set(Action::MenuRight, {key_bind(kVkRight)});
    return config;
}

bool validate_controls(const ControlsConfig& config, std::string* error) {
    const auto fail = [&](const char* what) {
        if (error != nullptr) *error = what;
        return false;
    };
    const auto in_range = [](double value) { return value >= kSensitivityMin && value <= kSensitivityMax; };
    if (!in_range(config.mouse_sensitivity_x)) return fail("mouse_sensitivity_x is outside 0.01 .. 50");
    if (!in_range(config.mouse_sensitivity_y)) return fail("mouse_sensitivity_y is outside 0.01 .. 50");
    if (config.walk_deflection < 1 || config.walk_deflection > kStickFull) return fail("walk_deflection is outside 1 .. 100");
    return true;
}

bool parse_controls(const std::string& text, ControlsConfig& config, std::vector<std::string>* errors) {
    bool clean = true;
    int number = 0;
    const auto report = [&](const std::string& what) {
        clean = false;
        if (errors != nullptr) errors->push_back("line " + std::to_string(number) + ": " + what);
    };
    std::size_t begin = 0;
    while (begin <= text.size()) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string::npos) end = text.size();
        std::string_view line(text.data() + begin, end - begin);
        begin = end + 1;
        ++number;
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            report("expected key = value");
            continue;
        }
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));

        Action action{};
        if (action_from_name(key, action)) {
            ActionBinds row{};
            bool ok = true;
            if (!iequals(value, "none")) {
                std::size_t slot = 0, cursor = 0;
                while (cursor <= value.size() && ok) {
                    std::size_t comma = value.find(',', cursor);
                    if (comma == std::string_view::npos) comma = value.size();
                    Bind bind;
                    if (slot >= kMaxBinds) {
                        report(std::string(key) + ": more than " + std::to_string(kMaxBinds) + " binds");
                        ok = false;
                    } else if (!parse_bind(value.substr(cursor, comma - cursor), bind)) {
                        report(std::string(key) + ": cannot read bind '" +
                               std::string(trim(value.substr(cursor, comma - cursor))) + "'");
                        ok = false;
                    } else {
                        row[slot++] = bind;
                    }
                    cursor = comma + 1;
                }
            }
            if (ok) config.binds[static_cast<std::size_t>(action)] = row;
            continue;
        }
        if (key == "mouse_sensitivity_x" || key == "mouse_sensitivity_y") {
            double number_value = 0.0;
            if (!parse_number(value, number_value) || number_value < kSensitivityMin || number_value > kSensitivityMax) {
                report(std::string(key) + ": expected a number from 0.01 to 50");
                continue;
            }
            (key == "mouse_sensitivity_x" ? config.mouse_sensitivity_x : config.mouse_sensitivity_y) = number_value;
            continue;
        }
        if (key == "mouse_invert_x" || key == "mouse_invert_y" || key == "mouse_capture") {
            bool flag = false;
            if (!parse_bool(value, flag)) {
                report(std::string(key) + ": expected 0 or 1");
                continue;
            }
            (key == "mouse_invert_x" ? config.mouse_invert_x
                                     : key == "mouse_invert_y" ? config.mouse_invert_y : config.mouse_capture) = flag;
            continue;
        }
        if (key == "walk_deflection") {
            int deflection = 0;
            if (!parse_number(value, deflection) || deflection < 1 || deflection > kStickFull) {
                report("walk_deflection: expected 1 to 100");
                continue;
            }
            config.walk_deflection = deflection;
            continue;
        }
        report("unknown key '" + std::string(key) + "'");
    }
    return clean;
}

std::vector<ControlsSetting> controls_settings(const ControlsConfig& config) {
    std::vector<ControlsSetting> out;
    for (std::size_t index = 0; index < kActionCount; ++index) {
        std::string value;
        for (const Bind& bind : config.binds[index]) {
            if (!bind.bound()) continue;
            if (!value.empty()) value += ", ";
            value += format_bind(bind);
        }
        out.push_back({kActions[index].name, value.empty() ? "none" : value});
    }
    const auto number = [](double value) {
        char text[32];
        std::snprintf(text, sizeof text, "%.17g", value);
        return std::string(text);
    };
    out.push_back({"mouse_sensitivity_x", number(config.mouse_sensitivity_x)});
    out.push_back({"mouse_sensitivity_y", number(config.mouse_sensitivity_y)});
    out.push_back({"mouse_invert_x", config.mouse_invert_x ? "1" : "0"});
    out.push_back({"mouse_invert_y", config.mouse_invert_y ? "1" : "0"});
    out.push_back({"mouse_capture", config.mouse_capture ? "1" : "0"});
    out.push_back({"walk_deflection", std::to_string(config.walk_deflection)});
    return out;
}

std::vector<Action> bound_actions(const ControlsConfig& config, Bind bind, Action ignore) {
    std::vector<Action> out;
    if (!bind.bound()) return out;
    for (std::size_t index = 0; index < kActionCount; ++index) {
        const Action action = static_cast<Action>(index);
        if (action == ignore) continue;
        if (std::find(config.binds[index].begin(), config.binds[index].end(), bind) != config.binds[index].end()) {
            out.push_back(action);
        }
    }
    return out;
}

ActionState resolve_actions(const ControlsConfig& config, const KeyMouseLevels& levels, bool in_menu) noexcept {
    ActionState state;
    for (std::size_t index = 0; index < kActionCount; ++index) {
        for (const Bind& bind : config.binds[index]) {
            if (in_menu && bind.source == Bind::Source::Mouse) continue;
            if (levels.down(bind)) {
                state.held[index] = true;
                state.any_kbm_input = true;
                break;
            }
        }
    }
    return state;
}

ControllerSample sample_from_actions(const ControlsConfig& config, const ActionState& actions, bool back_means_b) noexcept {
    ControllerSample sample;
    sample.connected = true;
    std::uint32_t controls = 0;
    for (std::size_t index = 0; index < kActionCount; ++index) {
        if (actions.held[index]) controls |= kActions[index].control;
    }
    if (actions.down(Action::Back)) controls |= back_means_b ? kControlB : kControlStart;
    sample.controls = controls;

    const auto axis = [](bool negative, bool positive, int full) {
        return static_cast<std::int8_t>((positive ? full : 0) - (negative ? full : 0));
    };
    const bool walk = actions.down(Action::Walk);
    const int x = axis(actions.down(Action::MoveLeft), actions.down(Action::MoveRight), 1);
    const int y = axis(actions.down(Action::MoveBack), actions.down(Action::MoveForward), 1);
    // The engine truncates each axis to 1/42 steps before its circle clamp, so a diagonal's length depends on the raw value:
    // 100 on both axes reads 0.976 or 0.993, and differently per quadrant. 75 (and -76, which reads as -75) is a value the
    // engine renormalises to exactly 1.0 on every diagonal (docs/controls/PLAYER_MOVEMENT_INPUT.md section 7).
    const int deflection = [&] {
        if (x == 0 || y == 0) return walk ? config.walk_deflection : kStickFull;
        if (!walk) return kFullDiagonal;
        // Same walking speed as a cardinal: each axis carries 1/sqrt(2) of the normalised deflection above the dead zone.
        const int above = config.walk_deflection - kLeftStickDead;
        return kLeftStickDead + std::max(1, static_cast<int>(above / 1.4142135623730951 + 0.5));
    }();
    const auto stick = [&](int direction) {
        return static_cast<std::int8_t>(direction > 0 ? deflection : direction < 0 ? -deflection - (x != 0 && y != 0 ? 1 : 0) : 0);
    };
    sample.stick_x = stick(x);
    sample.stick_y = stick(y);
    sample.substick_x = axis(actions.down(Action::CameraLeft), actions.down(Action::CameraRight), kStickFull);
    sample.substick_y = axis(actions.down(Action::CameraDown), actions.down(Action::CameraUp), kStickFull);
    return sample;
}

}  // namespace shadow::peripheral
