#include "gekko/ui_test.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace gekko {
#if !defined(GEKKO_SHIPPING)  // ES: the script parser is development-only
namespace {

std::string upper_copy(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool parse_buttons(const std::string& list, u32& out, std::string* error) {
    out = 0;
    std::string token;
    std::istringstream stream(list);
    while (std::getline(stream, token, ',')) {
        if (token.empty()) continue;
        shadow::peripheral::Control control{};
        if (!shadow::peripheral::control_from_name(upper_copy(token).c_str(), control)) {
            if (error != nullptr) *error = "unknown button '" + token + "'";
            return false;
        }
        out |= control;
    }
    return true;
}

bool parse_u32(const std::string& text, u32& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return false;
    out = static_cast<u32>(value);
    return true;
}

bool parse_s8(const std::string& text, s8& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || value < -128 || value > 127) return false;
    out = static_cast<s8>(value);
    return true;
}

}  // namespace

bool UiTestScript::parse(const std::string& text, UiTestScript& script, std::string* error) {
    script.steps_.clear();
    script.loop_ = false;
    script.total_frames_ = 0;
    script.last_step_ = static_cast<std::size_t>(-1);

    std::istringstream lines(text);
    std::string line;
    int number = 0;
    std::string pending_label;
    while (std::getline(lines, line)) {
        ++number;
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        std::istringstream fields(line);
        std::string command;
        if (!(fields >> command)) continue;
        command = upper_copy(command);

        const auto fail = [&](const std::string& why) {
            if (error != nullptr) {
                *error = "ui-test script line " + std::to_string(number) + ": " + why;
            }
            return false;
        };

        if (command == "LOOP") {
            script.loop_ = true;
            continue;
        }
        if (command == "LABEL") {
            std::string rest;
            std::getline(fields, rest);
            const auto begin = rest.find_first_not_of(" \t");
            pending_label = begin == std::string::npos ? std::string() : rest.substr(begin);
            continue;
        }

        UiTestStep step;
        if (command == "WAIT") {
            std::string frames;
            if (!(fields >> frames) || !parse_u32(frames, step.frames) || step.frames == 0) {
                return fail("wait needs a positive frame count");
            }
        } else if (command == "PRESS") {
            std::string buttons;
            std::string frames;
            if (!(fields >> buttons)) return fail("press needs a button list");
            if (!parse_buttons(buttons, step.controls, error)) {
                if (error != nullptr) {
                    *error = "ui-test script line " + std::to_string(number) + ": " + *error;
                }
                return false;
            }
            if (!(fields >> frames) || !parse_u32(frames, step.frames) || step.frames == 0) {
                return fail("press needs a positive frame count");
            }
        } else if (command == "STICK") {
            std::string x;
            std::string y;
            std::string frames;
            if (!(fields >> x >> y >> frames)) return fail("stick needs x y frames");
            if (!parse_s8(x, step.stick_x) || !parse_s8(y, step.stick_y)) {
                return fail("stick values must be -128..127");
            }
            if (!parse_u32(frames, step.frames) || step.frames == 0) {
                return fail("stick needs a positive frame count");
            }
        } else if (command == "CSTICK") {
            std::string x;
            std::string y;
            std::string frames;
            if (!(fields >> x >> y >> frames)) return fail("cstick needs x y frames");
            if (!parse_s8(x, step.substick_x) || !parse_s8(y, step.substick_y)) {
                return fail("cstick values must be -128..127");
            }
            if (!parse_u32(frames, step.frames) || step.frames == 0) {
                return fail("cstick needs a positive frame count");
            }
        } else {
            return fail("unknown command '" + command + "'");
        }

        step.label = pending_label;
        pending_label.clear();
        script.total_frames_ += step.frames;
        script.steps_.push_back(std::move(step));
    }

    if (script.steps_.empty()) {
        if (error != nullptr) *error = "ui-test script has no steps";
        return false;
    }
    return true;
}
#endif  // !GEKKO_SHIPPING

shadow::peripheral::ControllerSample UiTestScript::sample_for_frame(u32 vi_frame,
                                                                  const std::string** entered_step) {
    shadow::peripheral::ControllerSample sample;
    sample.connected = true;
    if (entered_step != nullptr) *entered_step = nullptr;
    if (steps_.empty() || total_frames_ == 0) return sample;

    u32 position = vi_frame;
    if (position >= total_frames_) {
        if (!loop_) {
            // Past the end of a one-shot script: hold nothing so the game is
            // left alone rather than being driven somewhere unintended.
            return sample;
        }
        position %= total_frames_;
    }

    u32 cursor = 0;
    for (std::size_t index = 0; index < steps_.size(); ++index) {
        const UiTestStep& step = steps_[index];
        if (position < cursor + step.frames) {
            sample.controls = step.controls;
            sample.stick_x = step.stick_x;
            sample.stick_y = step.stick_y;
            sample.substick_x = step.substick_x;
            sample.substick_y = step.substick_y;
            if (index != last_step_) {
                last_step_ = index;
                if (entered_step != nullptr && !step.label.empty()) {
                    *entered_step = &step.label;
                }
            }
            return sample;
        }
        cursor += step.frames;
    }
    return sample;
}

std::string UiTestScript::builtin_modes() {
    return "boot, fade, loading, select, hud";
}

#if !defined(GEKKO_SHIPPING)  // ES: the built-in scripts are development-only
bool UiTestScript::builtin(const std::string& mode, UiTestScript& script, std::string* error) {
    // These are starting points, not a recovered input map: the exact button
    // each screen wants is confirmed by running them and looking at the
    // filmstrip. --ui-test-script overrides any of them without a rebuild,
    // which is how they were derived in the first place.
    //
    // Every press is a short hold followed by a release so the script works
    // whether the menu acts on a rising edge or on held state.
    static const char* const kBoot =
        "# Advance past the boot dialogs and the title screen.\n"
        "label boot-wait\n"
        "wait 240\n"
        "label dismiss-dialog\n"
        "press A 8\n"
        "wait 40\n"
        "press START 8\n"
        "wait 40\n"
        "press A 8\n"
        "wait 120\n";

    static const char* const kFade =
        "# Sit on whatever screen the boot sequence reaches and repeatedly\n"
        "# confirm, so the fade in and out of a transition runs over and over.\n"
        "label settle\n"
        "wait 240\n"
        "label confirm\n"
        "press A 8\n"
        "label observe-fade\n"
        "wait 90\n"
        "press START 8\n"
        "wait 90\n"
        "loop\n";

    static const char* const kSelect =
        "# Boot to the mode/stage select and then move the cursor forever, so\n"
        "# the selector, the panels and their animations can all be observed.\n"
        "label boot\n"
        "wait 240\n"
        "press A 8\n"
        "wait 60\n"
        "press START 8\n"
        "wait 90\n"
        "press A 8\n"
        "wait 120\n"
        "label navigate\n"
        "press DOWN 6\n"
        "wait 24\n"
        "press UP 6\n"
        "wait 24\n"
        "press RIGHT 6\n"
        "wait 24\n"
        "press LEFT 6\n"
        "wait 24\n"
        "loop\n";

    static const char* const kLoading =
        "# Drive a stage load, watch the loading screen, then let it run into\n"
        "# gameplay. Long waits so the whole transition is captured.\n"
        "label boot\n"
        "wait 240\n"
        "press A 8\n"
        "wait 60\n"
        "press START 8\n"
        "wait 90\n"
        "label confirm-1\n"
        "press A 8\n"
        "wait 90\n"
        "label confirm-2\n"
        "press A 8\n"
        "wait 90\n"
        "label confirm-3\n"
        "press A 8\n"
        "label loading\n"
        "wait 600\n";

    static const char* const kHud =
        "# Gameplay HUD. Intended to be used with --load-state, so the boot\n"
        "# steps are skipped and the pad only exercises the HUD: move, then\n"
        "# pause and resume so the pause transition and the HUD entrance run.\n"
        "label idle\n"
        "wait 90\n"
        "label move\n"
        "stick 100 0 60\n"
        "stick -100 0 60\n"
        "label pause\n"
        "press START 8\n"
        "wait 120\n"
        "press START 8\n"
        "wait 120\n"
        "loop\n";

    const std::string lowered = [&] {
        std::string copy = mode;
        for (char& ch : copy) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return copy;
    }();

    const char* text = nullptr;
    if (lowered == "boot") text = kBoot;
    else if (lowered == "fade") text = kFade;
    else if (lowered == "loading") text = kLoading;
    else if (lowered == "select") text = kSelect;
    else if (lowered == "hud") text = kHud;
    if (text == nullptr) {
        if (error != nullptr) {
            *error = "unknown --ui-test mode '" + mode + "'; known modes: " + builtin_modes();
        }
        return false;
    }
    return parse(text, script, error);
}

#endif  // !GEKKO_SHIPPING
}  // namespace gekko
