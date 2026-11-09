#pragma once

// Development-only UI state driver.
//
// The fade, loading-screen, Select-menu and HUD layouts can only be inspected
// while the game is actually in those states, and this runtime has no automatic
// input by design (see PROJECT_STATE.md). Reaching a stage-load transition by
// hand, at seven resolutions, for every layout change, is not a workable loop.
//
// This drives the *original* transition code: it latches ordinary controller
// samples into the peripheral layer (port 0) exactly as the keyboard and XInput bridges
// do, paced by VI fields so it follows the game's own frame rate rather than
// host wall-clock. Nothing here interprets game state, patches memory or fakes
// a screen; if a script drives the game somewhere unexpected, what is captured
// is what the game really did.
//
// It is inert unless --ui-test or --ui-test-script names a script. No part of
// it runs on the normal path.

#include <cstdint>
#include <string>
#include <vector>

#include "peripheral/controls.h"
#include "gekko/types.h"

namespace gekko {

// One step of a script. `controls` are held for `frames` VI fields; a step
// with none is a wait. Analog stick values are signed -100..100 like the
// keyboard bridge produces.
struct UiTestStep {
    u32 controls = 0;  // the game's controls held (peripheral/controls.h)
    s8 stick_x = 0;
    s8 stick_y = 0;
    s8 substick_x = 0;  // the C stick (the camera)
    s8 substick_y = 0;
    u32 frames = 1;
    // Marks the frame this step starts on in the log, so a capture can be tied
    // back to the step that produced it.
    std::string label;
};

class UiTestScript {
public:
    // Parses the tiny line-oriented script format:
    //
    //   # comment
    //   wait 30                 advance 30 VI fields with no input
    //   press START 6           hold Start for 6 VI fields
    //   press A,UP 4            hold several buttons
    //   stick 0 -100 10         hold the analog stick for 10 fields
    //   cstick 100 0 30         hold the C stick (the camera) for 30 fields
    //   label enter-loading     name the following steps in the log
    //   loop                    restart from the first step
    //
    // Button names: A B X Y START Z L R UP DOWN LEFT RIGHT.
#if defined(GEKKO_SHIPPING)
    // GEKKO_SHIPPING (ES): development-only; --ui-test / --ui-test-script are refused at parse time.
    static bool parse(const std::string&, UiTestScript&, std::string* error) {
        if (error != nullptr) *error = "the UI-test driver is not part of this build";
        return false;
    }
#else
    static bool parse(const std::string& text, UiTestScript& script, std::string* error);
#endif

    // The built-in scripts behind --ui-test <mode>.
#if defined(GEKKO_SHIPPING)
    static bool builtin(const std::string&, UiTestScript&, std::string* error) {
        if (error != nullptr) *error = "the UI-test driver is not part of this build";
        return false;
    }
#else
    static bool builtin(const std::string& mode, UiTestScript& script, std::string* error);
#endif
    static std::string builtin_modes();

    // Advances to `vi_frame` and returns the controller sample to latch. Returns
    // the step label in `label` when the step changes, so the caller can log it.
    shadow::peripheral::ControllerSample sample_for_frame(u32 vi_frame, const std::string** entered_step);

    bool empty() const noexcept { return steps_.empty(); }
    bool looping() const noexcept { return loop_; }
    u32 total_frames() const noexcept { return total_frames_; }
    std::size_t step_count() const noexcept { return steps_.size(); }

private:
    std::vector<UiTestStep> steps_;
    bool loop_ = false;
    u32 total_frames_ = 0;
    std::size_t last_step_ = static_cast<std::size_t>(-1);
};

}  // namespace gekko
