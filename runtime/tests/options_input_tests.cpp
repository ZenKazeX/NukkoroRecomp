// Tests of the Options > Controls settings (Agent 4, 2026-11-05): camera inversion, rumble strength, persistence in
// options.cfg and the startup application through PcSettingsService.
//
// Sources to link (no CMake edits here - the harness owner wires them):
//   runtime/tests/options_input_tests.cpp runtime/src/gekko/xinput_pad.cpp runtime/src/gekko/pc_settings.cpp
//   runtime/src/gekko/config_file.cpp runtime/lift/peripheral/gamepad_binding.cpp
//   runtime/lift/peripheral/keyboard_layout.cpp runtime/lift/peripheral/controls.cpp
// include dirs: runtime/include runtime/lift; C++20. The test defines set_audio_output_gain / audio_output_gain
// itself (pc_settings.cpp pushes the master volume), so do NOT link audio_runtime.cpp into this target.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "gekko/audio_runtime.h"
#include "gekko/pc_settings.h"
#include "gekko/xinput_pad.h"
#include "peripheral/controls.h"
#include "peripheral/gamepad_binding.h"
#include "peripheral/keyboard_layout.h"
#include "test_scratch.h"

namespace gekko {
namespace {
float g_gain = 1.0f;
}
void set_audio_output_gain(float gain) noexcept { g_gain = gain; }
float audio_output_gain() noexcept { return g_gain; }
}  // namespace gekko

namespace {

using namespace shadow::peripheral;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

// ---- camera inversion -----------------------------------------------------------------------------------------

void inversion_negates_the_axis_and_saturates() {
    check(gekko::invert_stick_axis(0) == 0, "0 stays 0");
    check(gekko::invert_stick_axis(100) == -100, "+100 -> -100");
    check(gekko::invert_stick_axis(-100) == 100, "-100 -> +100");
    check(gekko::invert_stick_axis(1) == -1 && gekko::invert_stick_axis(-1) == 1, "+-1");
    check(gekko::invert_stick_axis(127) == -127, "+127 -> -127");
    check(gekko::invert_stick_axis(-127) == 127, "-127 -> +127");
    check(gekko::invert_stick_axis(-128) == 127, "-128 -> +127 (saturates; it must not wrap back to -128)");
    // Involution on the whole range the game can produce (+-100) and everything but -128.
    for (int v = -127; v <= 127; ++v) {
        const auto a = static_cast<std::int8_t>(v);
        if (gekko::invert_stick_axis(gekko::invert_stick_axis(a)) != a) {
            check(false, "inverting twice returns the value");
            break;
        }
    }
}

ControllerSample busy_sample() {
    ControllerSample s;
    s.connected = true;
    s.controls = kControlA | kControlStart | kControlL;
    s.stick_x = 33;
    s.stick_y = -44;
    s.substick_x = 77;
    s.substick_y = -55;
    return s;
}

void inversion_touches_only_the_camera_stick() {
    const ControllerSample base = busy_sample();
    ControllerSample none = gekko::apply_camera_inversion(base, false, false);
    check(none == base, "no inversion: the sample is unchanged");
    ControllerSample x = gekko::apply_camera_inversion(base, true, false);
    check(x.substick_x == -77 && x.substick_y == -55, "X only");
    check(x.stick_x == 33 && x.stick_y == -44 && x.controls == base.controls && x.connected, "X: nothing else moves");
    ControllerSample y = gekko::apply_camera_inversion(base, false, true);
    check(y.substick_x == 77 && y.substick_y == 55, "Y only");
    ControllerSample both = gekko::apply_camera_inversion(base, true, true);
    check(both.substick_x == -77 && both.substick_y == 55, "both");
    check(gekko::apply_camera_inversion(both, true, true) == base, "twice = original");
}

void inversion_does_not_touch_the_keyboard_or_a_merged_sample_twice() {
    // The keyboard has no camera stick (CONTROLS_PLAN.md): inversion cannot apply to it and must not invent one.
    const auto none = [](int, void*) { return false; };
    const auto all = [](int, void*) { return true; };
    ControllerSample idle = sample_from_keyboard(none, nullptr);
    ControllerSample pressed = sample_from_keyboard(all, nullptr);
    check(idle.substick_x == 0 && idle.substick_y == 0, "keyboard idle: no camera stick");
    check(pressed.substick_x == 0 && pressed.substick_y == 0, "keyboard, every key: still no camera stick");
    check(gekko::apply_camera_inversion(pressed, true, true).substick_x == 0, "inverting 0 stays 0");
    // The runtime inverts the PAD sample before combine(); the merged sample carries the inverted camera once.
    ControllerSample pad = busy_sample();
    const ControllerSample pad_inverted = gekko::apply_camera_inversion(pad, true, true);
    const ControllerSample merged = combine(pressed, pad_inverted);
    check(merged.substick_x == -77 && merged.substick_y == 55, "merge keeps the pad's inverted camera");
    // A disconnected pad leaves the keyboard alone.
    ControllerSample absent;
    check(combine(pressed, absent) == pressed, "no pad: keyboard unchanged");
}

void inversion_through_the_gamepad_binding() {
    GamepadState g;
    g.right_x = 32767;   // camera full right
    g.right_y = -32768;  // camera full down (i16 min)
    const GamepadBinding binding{};
    ControllerSample s = sample_from_gamepad(g, binding);
    check(s.substick_x == 100 && s.substick_y == -100, "full deflection maps to +-100 (the stick range is +-100)");
    ControllerSample inv = gekko::apply_camera_inversion(s, true, true);
    check(inv.substick_x == -100 && inv.substick_y == 100, "inverted full deflection");
    GamepadBinding swapped{};
    swapped.swap_sticks = true;  // the camera is then the LEFT stick
    g.left_x = 16000;
    ControllerSample w = sample_from_gamepad(g, swapped);
    check(w.substick_x == 48 && w.stick_x == 100, "swap_sticks: the camera is the left stick");
    check(gekko::apply_camera_inversion(w, true, false).substick_x == -48, "inversion follows the camera stick");
}

// ---- rumble strength ------------------------------------------------------------------------------------------

void rumble_scaling() {
    check(gekko::xinput_rumble_speed(false, 100) == 0 && gekko::xinput_rumble_speed(false, 25) == 0,
          "off = 0 at any strength");
    check(gekko::xinput_rumble_speed(true, 100) == 0xC000, "100 % = the original 0xC000");
    check(gekko::xinput_rumble_speed(true, 75) == 0x9000, "75 %");
    check(gekko::xinput_rumble_speed(true, 50) == 0x6000, "50 %");
    check(gekko::xinput_rumble_speed(true, 25) == 0x3000, "25 %");
    check(gekko::xinput_rumble_speed(true, 150) == 0xC000, "over 100 clamps to 100");
    check(gekko::xinput_rumble_speed(true, 0) == 0, "0 % = silent");
    unsigned previous = 0;
    bool monotonic = true;
    for (unsigned p = 25; p <= 100; ++p) {
        const unsigned v = gekko::xinput_rumble_speed(true, p);
        if (v < previous || v > 0xC000u) monotonic = false;
        previous = v;
    }
    check(monotonic, "25..100 % is monotonic and never above 0xC000");
}

// ---- persistence and the startup application ------------------------------------------------------------------

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream o;
    o << in.rdbuf();
    return o.str();
}

void write_file(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

void persistence_round_trip() {
    const std::filesystem::path dir = gekko::test_support::scratch_directory("options_input_tests");
    std::filesystem::create_directories(dir);
    const std::filesystem::path cfg = dir / "options.cfg";
    std::filesystem::remove(cfg);

    // Cold start: no file -> the original behaviour.
    {
        gekko::PcSettingsService s;
        s.load(cfg);
        check(s.current().rumble_strength == 100 && !s.current().invert_camera_x && !s.current().invert_camera_y,
              "defaults without a file");
    }
    // Change, apply, save.
    {
        gekko::PcSettingsService s;
        s.load(cfg);
        gekko::PcSettings p = s.current();
        p.rumble_strength = 50;
        p.invert_camera_x = true;
        p.invert_camera_y = true;
        std::string err;
        check(s.apply(p, &err), "apply accepts 50 / invert");
        check(s.save(&err), "save succeeds");
    }
    const std::string text = slurp(cfg);
    check(text.find("rumble_strength") != std::string::npos && text.find("invert_camera_x") != std::string::npos &&
              text.find("invert_camera_y") != std::string::npos,
          "options.cfg carries the three keys");
    // Restart: a new service loads the file; the values are restored.
    {
        gekko::PcSettingsService s;
        s.load(cfg);
        const gekko::PcSettings c = s.current();
        check(c.rumble_strength == 50 && c.invert_camera_x && c.invert_camera_y, "restored after restart");
        // load() pushes to the device layer: the camera inversion is then in effect for the next sample, which is
        // what the input thread latches (poll_xinput_controller reads the same atomics; set_xinput_preferences has
        // no getter, so the observable effect is the rumble speed below and the sample transform above).
    }
    // Restore defaults and persist them.
    {
        gekko::PcSettingsService s;
        s.load(cfg);
        gekko::PcSettings p = s.current();
        p.rumble_strength = gekko::PcSettings{}.rumble_strength;
        p.invert_camera_x = p.invert_camera_y = false;
        check(s.apply(p), "apply defaults");
        check(s.save(), "save defaults");
        gekko::PcSettingsService again;
        again.load(cfg);
        check(again.current() == p, "defaults restored and persisted");
    }
    // Invalid values: refused by apply, replaced by the default on load, and the rest of the file is kept.
    {
        gekko::PcSettingsService s;
        s.load(cfg);
        gekko::PcSettings p = s.current();
        std::string err;
        p.rumble_strength = 10;
        check(!s.apply(p, &err), "rumble 10 refused");
        p.rumble_strength = 101;
        check(!s.apply(p, &err), "rumble 101 refused");
        p.rumble_strength = -5;
        check(!s.apply(p, &err), "rumble -5 refused");
        check(s.current().rumble_strength == 100, "a refused apply changes nothing");
        write_file(cfg, "rumble_strength = 5\ninvert_camera_x = maybe\ninvert_camera_y = on\nmaster_volume = 40\n");
        gekko::PcSettingsService t;
        std::vector<std::string> warnings;
        t.load(cfg, &warnings);
        check(t.current().rumble_strength == 100, "out-of-range rumble in the file -> default");
        check(!t.current().invert_camera_x, "unparsable bool -> default");
        check(t.current().invert_camera_y && t.current().master_volume == 40, "valid neighbours still load");
        check(warnings.size() >= 2, "bad values are reported");
    }
    // Unknown keys and comments survive a save.
    {
        write_file(cfg, "# my notes\nfuture_key = 1\nrumble_strength = 75\n");
        gekko::PcSettingsService s;
        s.load(cfg);
        gekko::PcSettings p = s.current();
        p.invert_camera_y = true;
        check(s.apply(p) && s.save(), "save with unknown keys");
        const std::string kept = slurp(cfg);
        check(kept.find("# my notes") != std::string::npos && kept.find("future_key = 1") != std::string::npos,
              "comments and unknown keys are kept");
    }
    std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
    inversion_negates_the_axis_and_saturates();
    inversion_touches_only_the_camera_stick();
    inversion_does_not_touch_the_keyboard_or_a_merged_sample_twice();
    inversion_through_the_gamepad_binding();
    rumble_scaling();
    persistence_round_trip();
    if (g_failures == 0) std::printf("options_input_tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
