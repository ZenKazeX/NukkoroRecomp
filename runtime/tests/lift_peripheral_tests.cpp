// Tests of the lifted peripheral layer (docs/lift-20261006/L1_INPUT.md): the PC implementation of the
// engine's peripheral interface, the gamepad and keyboard mappings, and its persistence. Pure native code:
// nothing here knows the translated machine.

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "peripheral/controls.h"
#include "peripheral/gamepad_binding.h"
#include "peripheral/keyboard_layout.h"
#include "peripheral/manager_imp_pc.h"

namespace {

using namespace shadow::peripheral;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

// A connected port reads Ready with its controls and both sticks in the engine's convention: y down, a
// negative deflection one unit closer to centre (the shipped game's reported values: -100 reads -99).
void read_reports_the_engine_convention() {
    ManagerImpPc imp;
    ControllerSample sample;
    sample.connected = true;
    sample.controls = kControlA | kControlStart | kControlLeft | 0x0004u | 0x0800u | 0x10000u;
    sample.stick_x = -100;
    sample.stick_y = 100;
    sample.substick_x = 30;
    sample.substick_y = -128;
    imp.latch(0, sample);
    ManagerImpPc::PortReading readings[ManagerImpPc::kPorts];
    imp.read(readings, ManagerImpPc::kPorts);
    const ManagerImpPc::PortReading& port0 = readings[0];
    check(port0.state == ManagerImpPc::PortState::Ready, "port 0 is ready");
    check(port0.controls == (kControlA | kControlStart | kControlLeft),
          "only the game's controls are reported (bits outside the set are dropped)");
    check(port0.left_x == -99 && port0.left_y == -100, "movement stick: -100 reads -99, up 100 reads -100 (y down)");
    check(port0.right_x == 30 && port0.right_y == 127, "camera stick: 30 as is, -128 reads 127 after the flip");
    for (std::size_t port = 1; port < ManagerImpPc::kPorts; ++port) {
        check(readings[port].state == ManagerImpPc::PortState::Absent && readings[port].controls == 0u,
              "ports without a controller read absent");
    }
    check(imp.stats().frames_read == 1u, "one frame read");
    ManagerImpPc::PortReading beyond[6];
    imp.read(beyond, 6);
    check(beyond[4].state == ManagerImpPc::PortState::Absent && beyond[5].state == ManagerImpPc::PortState::Absent,
          "a port past the fourth reads absent");
}

void start_state_and_reset() {
    ManagerImpPc imp;
    check(imp.port_count() == 4u, "four player ports");
    check(imp.connected_mask() == 0x1u, "port 0 starts connected (the keyboard), the others empty");
    ControllerSample pad;
    pad.connected = true;
    imp.latch(2, pad);
    check(imp.connected_mask() == 0x5u, "a latched controller connects its port");
    imp.latch(2, ControllerSample{});
    check(imp.connected_mask() == 0x1u, "an empty sample disconnects it - no probe, no delay");
    std::vector<std::pair<std::size_t, bool>> events;
    imp.set_rumble_listener([&](std::size_t port, bool on) { events.emplace_back(port, on); });
    imp.set_rumble(1, true);
    imp.reset();
    check(imp.connected_mask() == 0x1u && !imp.rumble(1), "reset restores the start state");
    check(events.size() == 2u && events[1] == std::make_pair(std::size_t{1}, false),
          "reset tells the device layer the rumble stopped");
}

void stick_range_is_the_engines_calibration() {
    ManagerImpPc imp;
    for (std::size_t port = 0; port < ManagerImpPc::kPorts; ++port) {
        const ManagerImpPc::StickRange range = imp.stick_range(port);
        check(range.left_dead == 15 && range.right_dead == 25 && range.left_range == 42 && range.right_range == 39,
              "dead zone 15 / 25 and live range 42 / 39 stick units");
    }
}

void rumble_requests_report_changes_only() {
    ManagerImpPc imp;
    std::vector<std::pair<std::size_t, bool>> events;
    imp.set_rumble_listener([&](std::size_t port, bool on) { events.emplace_back(port, on); });
    imp.set_rumble(0, true);
    imp.set_rumble(0, true);
    imp.set_rumble(3, true);
    imp.set_rumble(0, false);
    imp.set_rumble(4, true);
    check(events.size() == 3u, "three changes; a repeat and a fifth port are not reported");
    check(imp.rumble(3) && !imp.rumble(0) && imp.stats().rumble_changes == 3u, "the requests are kept");
}

void persistence_round_trips() {
    ManagerImpPc source;
    ControllerSample sample;
    sample.connected = true;
    sample.controls = kControlX | kControlR;
    sample.stick_x = -5;
    sample.substick_y = 77;
    source.latch(1, sample);
    source.set_rumble(1, true);
    std::vector<std::uint8_t> block;
    source.save(block);
    ManagerImpPc target;
    std::vector<std::pair<std::size_t, bool>> events;
    target.set_rumble_listener([&](std::size_t port, bool on) { events.emplace_back(port, on); });
    const std::uint8_t* cursor = block.data();
    check(target.load(cursor, block.data() + block.size()) && cursor == block.data() + block.size(),
          "the block loads whole");
    check(target.sample(1) == sample && target.sample(0).connected && target.rumble(1), "the loaded state is the saved one");
    check(events.size() == 1u && events[0] == std::make_pair(std::size_t{1}, true), "the restored rumble reaches the device");
    std::vector<std::uint8_t> corrupt = block;
    // Block layout: format byte, then per port: connected, controls (u32, little end first), 4 stick bytes,
    // rumble. 0x0004 is not one of the game's controls.
    corrupt[2] |= 0x04u;
    ManagerImpPc refused;
    cursor = corrupt.data();
    check(!refused.load(cursor, corrupt.data() + corrupt.size()), "controls outside the game's set are refused");
    corrupt = block;
    corrupt[0] = 2u;
    cursor = corrupt.data();
    check(!refused.load(cursor, corrupt.data() + corrupt.size()), "an unknown format is refused");
    cursor = block.data();
    check(!refused.load(cursor, block.data() + block.size() - 1u), "a truncated block is refused");
}

// The default binding: every gamepad button to its control, analog triggers past the threshold hold L / R,
// the left stick moves, the right stick turns the camera, 8000 / 32767 dead zone, x100 / 32767, +-100.
void gamepad_maps_to_the_games_controls() {
    GamepadState state;
    state.buttons = kGamepadDpadUp | kGamepadDpadDown | kGamepadDpadLeft | kGamepadDpadRight | kGamepadStart |
                    kGamepadLeftShoulder | kGamepadRightShoulder | kGamepadSouth | kGamepadEast | kGamepadWest |
                    kGamepadNorth;
    state.left_trigger = 40u;
    state.right_trigger = 41u;
    state.left_x = 32767;
    state.left_y = -32768;
    state.right_x = 7999;
    state.right_y = -7999;
    const ControllerSample sample = sample_from_gamepad(state, GamepadBinding{});
    check(sample.connected, "a polled gamepad is connected");
    check(sample.controls == kAllControls, "every button raises its control; the right trigger past 40 raises R");
    check(sample.stick_x == 100 && sample.stick_y == -100, "full deflection is 100 stick units");
    check(sample.substick_x == 0 && sample.substick_y == 0, "inside the dead zone is neutral");
    const ControllerSample neutral = sample_from_gamepad(GamepadState{}, GamepadBinding{});
    check(neutral.connected && neutral.controls == 0u && neutral.stick_x == 0 && neutral.stick_y == 0 &&
              neutral.substick_x == 0 && neutral.substick_y == 0,
          "a neutral gamepad holds nothing");
    GamepadBinding swapped;
    swapped.swap_sticks = true;
    GamepadState right_only;
    right_only.right_x = 16384;
    const ControllerSample moved = sample_from_gamepad(right_only, swapped);
    check(moved.stick_x == 50 && moved.substick_x == 0, "swap_sticks: the right stick moves");
    check(stick_units(8000) == 24 && stick_units(7999) == 0 && stick_units(-8000) == -24,
          "the dead zone edge: 7999 is 0, 8000 is 24");
}

bool key_held(int key, void* context) {
    const std::vector<int>& held = *static_cast<const std::vector<int>*>(context);
    for (const int k : held) {
        if (k == key) return true;
    }
    return false;
}

void keyboard_layout() {
    std::vector<int> held{'X', 'S', 0x20, 'C', 0x25, 'I'};
    const ControllerSample sample = sample_from_keyboard(key_held, &held);
    check(sample.connected, "the keyboard is always connected");
    check(sample.controls == (kControlA | kControlX | kControlStart | kControlZ | kControlLeft),
          "X = A, S = X, Space = Start, C = Z, Left = d-pad left");
    check(sample.stick_x == -100 && sample.stick_y == 100, "Left and I deflect the movement stick fully");
    held = {0x0D, 0x10};
    check(sample_from_keyboard(key_held, &held).controls == kControlStart, "Shift+Enter is Start, not A");
    held = {0x0D};
    check(sample_from_keyboard(key_held, &held).controls == kControlA, "Enter alone is A");
    held = {'J', 'L'};
    check(sample_from_keyboard(key_held, &held).stick_x == 0, "opposite keys cancel");
    ControllerSample gamepad;
    gamepad.connected = true;
    gamepad.controls = kControlB;
    gamepad.stick_y = -40;
    gamepad.substick_x = 12;
    held = {'X', 'J'};
    const ControllerSample both = combine(sample_from_keyboard(key_held, &held), gamepad);
    check(both.controls == (kControlA | kControlB), "controls from either device");
    check(both.stick_x == -100 && both.stick_y == -40 && both.substick_x == 12,
          "the gamepad's deflected axes win, the camera stick is the gamepad's");
    check(combine(sample_from_keyboard(key_held, &held), ControllerSample{}) == sample_from_keyboard(key_held, &held),
          "an absent gamepad leaves the keyboard's sample");
}

void control_names() {
    Control control{};
    check(control_from_name("START", control) && control == kControlStart, "START names Start");
    check(control_from_name("Z", control) && control == kControlZ, "Z names Z");
    check(!control_from_name("start", control) && !control_from_name("C", control) && !control_from_name(nullptr, control),
          "unknown or lower-case names are refused");
    check(std::string(control_name(kControlR)) == "R" && control_name(kControlA | kControlB) == nullptr,
          "one control has one name");
}

}  // namespace

int main() {
    read_reports_the_engine_convention();
    start_state_and_reset();
    stick_range_is_the_engines_calibration();
    rumble_requests_report_changes_only();
    persistence_round_trips();
    gamepad_maps_to_the_games_controls();
    keyboard_layout();
    control_names();
    if (g_failures == 0) {
        std::printf("lift_peripheral_tests: all passed\n");
        return 0;
    }
    std::printf("lift_peripheral_tests: %d failure(s)\n", g_failures);
    return 1;
}
