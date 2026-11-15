// The native keyboard and mouse layer: actions, config text, the settings service, and the mouse's camera arithmetic.
//
// What this suite holds:
//   1. The default layout binds what docs/controls/KEYBOARD_MOUSE_BINDINGS.md says, and every game control is reachable.
//   2. Actions resolve from levels: held, released, several at once, opposite directions cancel, alternates work.
//   3. controls.cfg: round trip, bad lines reported and not applied, the rest still applies, unknown keys are errors.
//   4. The mouse: rotation is a function of the counts only, never of how they were split across frames; slow motion is
//      not lost; a flick does not leave a backlog; only what the engine read is consumed; a reset forgets everything.
//   5. The stick written for a rate normalises back to that rate through the engine's own dead zone and range.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "gekko/controls_settings.h"
#include "peripheral/action_map.h"
#include "peripheral/manager_imp_pc.h"
#include "peripheral/mouse_look.h"

using namespace shadow::peripheral;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

KeyMouseLevels keys(std::initializer_list<int> held, std::uint8_t buttons = 0) {
    KeyMouseLevels levels;
    for (const int key : held) levels.keys.set(static_cast<std::size_t>(key));
    levels.mouse_buttons = buttons;
    return levels;
}

ControllerSample sample_for(const ControlsConfig& config, const KeyMouseLevels& levels) {
    return sample_from_actions(config, resolve_actions(config, levels));
}

constexpr int kVkSpace = 0x20, kVkEscape = 0x1B, kVkLShift = 0xA0, kVkLCtrl = 0xA2;

void default_layout() {
    const ControlsConfig config = default_controls();
    std::string error;
    check(validate_controls(config, &error), "the default layout validates: " + error);
    check(sample_for(config, keys({'W'})).stick_y == 100, "W is forward");
    check(sample_for(config, keys({'S'})).stick_y == -100, "S is back");
    check(sample_for(config, keys({'A'})).stick_x == -100, "A is left");
    check(sample_for(config, keys({'D'})).stick_x == 100, "D is right");
    check(sample_for(config, keys({kVkSpace})).controls == kControlA, "Space is the jump control");
    check(sample_for(config, keys({}, 1u << kMouseLeft)).controls == kControlB, "left mouse is the attack control");
    check(sample_for(config, keys({}, 1u << kMouseRight)).controls == kControlY, "right mouse is the Chaos control");
    check(sample_for(config, keys({'E'})).controls == kControlX, "E is the action control");
    check(sample_for(config, keys({'F'})).controls == kControlY, "F is the Chaos control");
    check(sample_for(config, keys({kVkLShift})).controls == kControlR, "Left Shift is parallel movement");
    check(sample_for(config, keys({kVkEscape})).controls == kControlStart, "Esc pauses during play");
    {
        const ActionState menu = resolve_actions(config, keys({kVkEscape}), true);
        check(sample_from_actions(config, menu, true).controls == kControlB, "Esc goes back in a menu");
        const ActionState click = resolve_actions(config, keys({}, 3u), true);
        check(sample_from_actions(config, click, true).controls == 0u, "mouse buttons do nothing in a menu");
        const ActionState paused = resolve_actions(config, keys({kVkEscape}), true);
        check(sample_from_actions(config, paused, false).controls == kControlStart, "Esc leaves the pause screen");
    }

    std::uint32_t reachable = 0;
    for (std::size_t index = 0; index < kActionCount; ++index) {
        const Action action = static_cast<Action>(index);
        if (action_control(action) == 0u) continue;
        check(config.binds[index][0].bound(), std::string(action_name(action)) + " has a default bind");
        reachable |= action_control(action);
    }
    check(reachable == kAllControls, "every control the game reads has an action");
}

void resolving() {
    const ControlsConfig config = default_controls();
    check(sample_for(config, keys({})).controls == 0u && sample_for(config, keys({})).stick_x == 0, "nothing held");
    check(sample_for(config, keys({})).connected, "the keyboard is always connected");

    const ControllerSample diagonal = sample_for(config, keys({'W', 'D'}));
    check(diagonal.stick_x == 75 && diagonal.stick_y == 75, "a diagonal uses the raw value the engine reads as length 1");
    const ControllerSample back_left = sample_for(config, keys({'S', 'A'}));
    check(back_left.stick_x == -76 && back_left.stick_y == -76, "-76 reads as -75: the same length in every quadrant");
    const ControllerSample walk_diagonal = sample_for(config, keys({'W', 'D', kVkLCtrl}));
    check(walk_diagonal.stick_x == 30 && walk_diagonal.stick_y == 30, "walking diagonals keep the walking magnitude");
    check(sample_for(config, keys({'A', 'D'})).stick_x == 0, "opposite keys cancel");
    check(sample_for(config, keys({'W', 'S', 'D'})).stick_y == 0 && sample_for(config, keys({'W', 'S', 'D'})).stick_x == 100,
          "a cancelled axis leaves the other");

    const ControllerSample walking = sample_for(config, keys({'W', kVkLCtrl}));
    check(walking.stick_y == config.walk_deflection, "Walk holds the walk deflection");

    const ControllerSample many = sample_for(config, keys({'W', 'A', kVkSpace, 'E', 'Q', kVkLShift, 'F'}, 1u));
    check(many.controls == (kControlA | kControlB | kControlX | kControlY | kControlL | kControlR),
          "simultaneous actions all register");
    check(many.stick_x == -76 && many.stick_y == 75, "movement during other actions");

    ControlsConfig alternate = config;
    alternate.binds[static_cast<std::size_t>(Action::Jump)][1] = key_bind('J');
    check(sample_for(alternate, keys({'J'})).controls == kControlA, "a second binding raises the action");
    check(sample_for(alternate, keys({kVkSpace, 'J'})).controls == kControlA, "both bindings at once are one control");

    ControlsConfig camera = config;
    camera.binds[static_cast<std::size_t>(Action::CameraRight)][0] = key_bind('L');
    check(sample_for(camera, keys({'L'})).substick_x == 100, "a camera key deflects the camera stick");

    check(bound_actions(config, key_bind('W')).size() == 1, "a conflict search finds the action");
    check(bound_actions(config, key_bind('W'), Action::MoveForward).empty(), "the ignored action is not a conflict");
}

void config_text() {
    ControlsConfig config = default_controls();
    std::vector<std::string> errors;
    check(parse_controls("jump = key:J, mouse:X1\n# comment\n\nmouse_sensitivity_x = 2.5\nmouse_invert_y = 1\nwalk_deflection = 40\n",
                         config, &errors),
          "a valid file parses");
    const ActionBinds& jump = config.binds[static_cast<std::size_t>(Action::Jump)];
    check(jump[0] == key_bind('J') && jump[1] == mouse_bind(kMouseX1) && !jump[2].bound(), "binds load in order");
    check(config.mouse_sensitivity_x == 2.5 && config.mouse_invert_y && config.walk_deflection == 40, "values load");

    ControlsConfig none = default_controls();
    check(parse_controls("jump = none\n", none, nullptr), "none is valid");
    check(!none.binds[static_cast<std::size_t>(Action::Jump)][0].bound(), "none unbinds");

    ControlsConfig bad = default_controls();
    errors.clear();
    check(!parse_controls("jump = key:Nonsense\nattack = key:K\nbogus = 1\nmouse_sensitivity_x = 999\nfire\n", bad, &errors),
          "problems are reported");
    check(errors.size() == 4, "one message per problem");
    check(errors[0].find("line 1") != std::string::npos, "the message names the line");
    check(bad.binds[static_cast<std::size_t>(Action::Jump)][0] == key_bind(kVkSpace), "a bad line is not applied");
    check(bad.binds[static_cast<std::size_t>(Action::Attack)][0] == key_bind('K'), "a good line still applies");
    check(bad.mouse_sensitivity_x == 1.0, "an out-of-range number is not applied");

    // Round trip through what the settings service writes.
    ControlsConfig source = default_controls();
    source.mouse_sensitivity_y = 0.3;
    source.mouse_capture = false;
    source.binds[static_cast<std::size_t>(Action::Chaos)][2] = key_bind(0x7C);
    std::string text;
    for (const ControlsSetting& row : controls_settings(source)) text += row.key + " = " + row.value + "\n";
    ControlsConfig back;
    errors.clear();
    check(parse_controls(text, back, &errors), "written text reads back");
    check(back == source, "round trip keeps every setting");

    for (int key = 1; key < 0xFF; ++key) {
        int read = 0;
        check(key_from_name(key_name(key), read) && read == key, "key name round trip for " + key_name(key));
    }
}

void settings_service() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "shadow_input_actions_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / "controls.cfg";
    std::filesystem::remove(path);

    gekko::ControlsSettings service;
    std::vector<std::string> warnings;
    service.load(path, &warnings);
    check(warnings.empty(), "a missing file is the defaults, without complaint");
    check(*service.current() == default_controls(), "defaults loaded");

    ControlsConfig changed = default_controls();
    changed.mouse_sensitivity_x = 4.0;
    const std::uint64_t generation = service.generation();
    std::string error;
    check(service.apply(changed, &error), "apply: " + error);
    check(service.generation() != generation, "apply publishes a new generation");
    check(service.save(&error), "save: " + error);
    check(std::filesystem::exists(path), "save creates the file");

    gekko::ControlsSettings fresh;
    fresh.load(path, &warnings);
    check(fresh.current()->mouse_sensitivity_x == 4.0, "a saved setting survives a restart");

    // Live rebinding: the file changes, the next check picks it up.
    {
        std::ofstream out(path, std::ios::app);
        out << "jump = key:J\n";
    }
    std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds(5));
    check(fresh.reload_if_changed(&warnings), "a changed file is noticed");
    check(fresh.current()->binds[static_cast<std::size_t>(Action::Jump)][0] == key_bind('J'), "the rebind applied");
    check(!fresh.reload_if_changed(&warnings), "an unchanged file is not reloaded");

    ControlsConfig invalid = default_controls();
    invalid.walk_deflection = 0;
    check(!fresh.apply(invalid, &error), "validation refuses a bad config");
    fresh.restore_defaults();
    check(*fresh.current() == default_controls(), "restore defaults");
    std::filesystem::remove_all(dir);
}

// What the game does with one rate: degrees of yaw over a frame.
double degrees_for_rate(double rate) { return rate * MouseLook::kFullRateDegreesPerSecond * MouseLook::kFrameSeconds; }

double rotate_with_splits(const std::vector<std::int64_t>& per_frame) {
    MouseLook look;
    double degrees = 0.0;
    std::int64_t total = 0;
    for (const std::int64_t counts : per_frame) {
        total += counts;
        degrees += degrees_for_rate(look.peek(total, 0).yaw_rate);
        look.commit();
    }
    return degrees;
}

void mouse_rotation() {
    const double expected = 600.0 * MouseLook::kDegreesPerCount;
    const double close = 1e-9;

    // The same 600 counts cut into frames several different ways, all below the rate limit.
    const double even = rotate_with_splits(std::vector<std::int64_t>(60, 10));
    check(std::abs(even - expected) < close, "600 counts evenly over a second");
    std::vector<std::int64_t> bursts;
    for (int i = 0; i < 12; ++i) {
        bursts.push_back(30);
        bursts.push_back(0);
        bursts.push_back(20);
        bursts.push_back(0);
    }
    check(std::abs(rotate_with_splits(bursts) - expected) < close, "the same counts in bursts: the same rotation");
    std::mt19937 random(12345);
    for (int trial = 0; trial < 200; ++trial) {
        std::vector<std::int64_t> frames;
        std::int64_t left = 600;
        while (left > 0) {
            const std::int64_t counts = std::min<std::int64_t>(left, std::uniform_int_distribution<int>(1, 40)(random));
            frames.push_back(counts);
            left -= counts;
        }
        check(std::abs(rotate_with_splits(frames) - expected) < close, "random frame splits keep the rotation");
    }

    // Counts too slow to register alone must not vanish: one count per frame still turns the camera.
    {
        MouseLook look;
        look.set_settings({0.05, 1.0, false, false});
        double degrees = 0.0;
        std::int64_t total = 0;
        int empty_frames = 0;
        for (int frame = 0; frame < 100; ++frame) {
            total += 1;
            const double rate = look.peek(total, 0).yaw_rate;
            if (rate == 0.0) ++empty_frames;
            degrees += degrees_for_rate(rate);
            look.commit();
        }
        check(empty_frames > 0, "this motion is below the game's steering threshold per frame");
        const double owed = 100.0 * MouseLook::kDegreesPerCount * 0.05;
        // At most one threshold's worth is still waiting at the end.
        const double waiting_limit = MouseLook::kMinimumRate * MouseLook::kFullRateDegreesPerSecond * MouseLook::kFrameSeconds;
        check(degrees <= owed + 1e-9 && owed - degrees <= waiting_limit, "slow motion accumulates instead of being lost");
    }

    // A flick above the camera's top speed plays at top speed and leaves nothing behind.
    {
        MouseLook look;
        look.set_settings({10.0, 1.0, false, false});
        const MouseLook::Output first = look.peek(1000, 0);
        look.commit();
        check(first.yaw_rate == 1.0, "a flick is limited to the camera's full input");
        const MouseLook::Output after = look.peek(1000, 0);
        check(!after.any(), "no backlog once the hand has stopped");
    }

    // Only what the engine read is consumed.
    {
        MouseLook look;
        const double rate_for_10 = look.peek(10, 0).yaw_rate;
        const double rate_for_30 = look.peek(30, 0).yaw_rate;  // a newer latch before any read
        check(rate_for_30 > rate_for_10, "a later peek includes the newer counts");
        look.commit();
        check(!look.peek(30, 0).any(), "counts the engine read are spent");
    }
    {
        MouseLook look;
        (void)look.peek(30, 0);
        // No commit: the engine did not read; the counts are still owed.
        check(look.peek(30, 0).any(), "unread counts stay owed");
    }

    // Reset (focus lost, game paused): nothing is owed, so nothing jumps when the mouse returns.
    {
        MouseLook look;
        (void)look.peek(500, 0);
        look.reset(500, 0);
        check(!look.peek(500, 0).any(), "a reset forgets the counts");
        check(look.peek(510, 0).any(), "new motion after a reset counts");
    }

    // Inversion and sign.
    {
        MouseLook normal, inverted;
        inverted.set_settings({1.0, 1.0, true, true});
        check(normal.peek(50, 50).yaw_rate > 0.0 && inverted.peek(50, 50).yaw_rate < 0.0, "invert X flips yaw");
        check(normal.peek(50, 50).height_rate > 0.0 && inverted.peek(50, 50).height_rate < 0.0, "invert Y flips height");
    }
}

// The engine's reading of a right-stick axis (ManagerImpPc::read + the stick range it hands the game).
double engine_normalised(std::int8_t value) {
    const ManagerImpPc::StickRange range{};
    int reading = value < 0 ? value + 1 : value;
    const int magnitude = std::abs(reading);
    if (magnitude <= range.right_dead) return 0.0;
    const double normalised = std::min<double>(1.0, static_cast<double>(magnitude - range.right_dead) / range.right_range);
    return reading < 0 ? -normalised : normalised;
}

void stick_mapping() {
    for (double rate = -1.0; rate <= 1.0; rate += 1.0 / 256.0) {
        if (rate == 0.0) continue;
        ControllerSample sample;
        apply_look_to_stick(sample, {rate, 0.0});
        const double read = engine_normalised(sample.substick_x);
        check(read != 0.0, "any rate is outside the dead zone");
        check((read > 0.0) == (rate > 0.0), "the sign survives");
        check(std::abs(read - rate) <= 0.5 / 39.0 + 1.0 / 39.0, "the rate survives within one stick step");
        check(std::abs(read * read) >= 1e-4, "the game counts this as steering");
    }
    ControllerSample full;
    apply_look_to_stick(full, {1.0, -1.0});
    check(engine_normalised(full.substick_x) == 1.0 && engine_normalised(full.substick_y) == -1.0, "full input is full stick");
    ControllerSample untouched;
    untouched.substick_x = 77;
    apply_look_to_stick(untouched, {});
    check(untouched.substick_x == 77, "no mouse motion leaves the gamepad's stick alone");
}

// What the input thread adds per poll (printed, not asserted: it is a measurement).
void action_cost() {
    const ControlsConfig config = default_controls();
    const KeyMouseLevels levels = keys({'W', 'D', kVkSpace}, 1u);
    constexpr int kRounds = 2'000'000;
    int sink = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int round = 0; round < kRounds; ++round) sink += sample_from_actions(config, resolve_actions(config, levels)).stick_x;
    const auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    std::printf("input_actions_tests: resolve_actions + sample_from_actions: %.1f ns per poll (%d)\n", elapsed / kRounds, sink != 0);
}

}  // namespace

int main() {
    default_layout();
    resolving();
    config_text();
    settings_service();
    mouse_rotation();
    action_cost();
    stick_mapping();
    if (g_failures == 0) {
        std::printf("input_actions_tests: all passed\n");
        return 0;
    }
    std::printf("input_actions_tests: %d failure(s)\n", g_failures);
    return 1;
}
