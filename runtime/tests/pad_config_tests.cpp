// The button mapping configurator's logic against the game's own pad.cfg parser (tools/padconfig/pad_config.h).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "pad_config.h"
#include "test_scratch.h"

using namespace padconfig;
namespace sp = shadow::peripheral;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

std::string read_all(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

GamepadBinding read_back(const GamepadBinding& b) {
    GamepadBinding out;
    std::string error;
    check(sp::parse_gamepad_binding(format_binding(b), out, &error), "the game's parser accepts what is written");
    return out;
}

}  // namespace

int main() {
    // Every setting survives a round trip through the game's parser.
    {
        GamepadBinding d;
        check(same(read_back(d), d), "the defaults round-trip");
        GamepadBinding c;
        c.a = sp::kGamepadNorth;
        c.b = 0;
        c.r = sp::kGamepadBack;
        c.trigger_threshold = 200;
        c.swap_sticks = true;
        c.button_style = sp::ButtonStyle::PlayStation;
        check(same(read_back(c), c), "a changed mapping round-trips");
        for (const sp::ButtonStyle s : {sp::ButtonStyle::Auto, sp::ButtonStyle::Original, sp::ButtonStyle::Xbox,
                                        sp::ButtonStyle::PlayStation, sp::ButtonStyle::Switch}) {
            c.button_style = s;
            check(same(read_back(c), c), "every prompt style round-trips");
        }
    }
    // A key the game has that the configurator does not list would be lost: all twelve are listed, each once.
    {
        std::string text;
        for (const Control& c : kControls) text += std::string(c.key) + " = 0x8000\n";
        GamepadBinding b, expected;
        check(sp::parse_gamepad_binding(text, b, nullptr), "each key is one the game knows");
        for (const Control& c : kControls) expected.*c.field = 0x8000;
        check(same(b, expected), "all twelve keys reach their own control");
    }
    // Choosing a button.
    {
        check(newly_pressed(0, sp::kGamepadSouth) == sp::kGamepadSouth, "a new press is seen");
        check(newly_pressed(sp::kGamepadSouth, sp::kGamepadSouth) == 0, "a button already held does not count");
        check(newly_pressed(sp::kGamepadSouth, sp::kGamepadSouth | sp::kGamepadEast) == sp::kGamepadEast, "only the new one counts");
        check(newly_pressed(0, 0) == 0, "nothing pressed");
        check(newly_pressed(0, 0x0400) == 0, "a bit that is not a button is ignored");
    }
    // A button does one job, and a swap leaves nothing empty.
    {
        GamepadBinding b;
        assign(b, 0, sp::kGamepadWest);  // A takes B's button
        check(b.a == sp::kGamepadWest && b.b == sp::kGamepadSouth, "assigning a used button swaps the two");
        b = GamepadBinding{};
        assign(b, 6, sp::kGamepadSouth);  // R (unbound) takes A's button
        check(b.r == sp::kGamepadSouth && b.a == 0, "taking from a control when there is nothing to give leaves it unset");
        b = GamepadBinding{};
        b.a = sp::kGamepadSouth | sp::kGamepadWest;
        assign(b, 3, sp::kGamepadWest);  // Y takes one of A's two buttons
        check(b.a == sp::kGamepadSouth && b.y == sp::kGamepadWest, "a control with two buttons keeps the other");
        b = GamepadBinding{};
        assign(b, 0, 0);
        check(b.a == 0 && b.b == sp::kGamepadWest, "clearing a control changes no other");
        const GamepadBinding before;
        b = GamepadBinding{};
        assign(b, 0, b.a);
        check(same(b, before), "assigning a control its own button changes nothing");
    }
    // The default layout is by position: the GameCube pad's B (left of A) on the west button, its X (right) on the east.
    {
        const GamepadBinding d;
        check(d.a == sp::kGamepadSouth && d.b == sp::kGamepadWest && d.x == sp::kGamepadEast && d.y == sp::kGamepadNorth,
              "the default maps the face buttons by position, B and X swapped against the letters");
    }
    // What is shown.
    {
        check(button_names(0) == "Not set", "unset");
        check(button_names(sp::kGamepadLeftShoulder) == "LB", "a button's name");
        check(button_names(sp::kGamepadSouth | sp::kGamepadEast) == "A + B", "two buttons");
    }
    // The file.
    {
        const auto dir = gekko::test_support::scratch_directory("pad-config-tests");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir);
        const auto file = dir / "pad.cfg";
        GamepadBinding b;
        std::string error;
        check(load_binding(file, b, &error) == LoadResult::Absent && same(b, GamepadBinding{}), "no file: the defaults");
        b.z = sp::kGamepadBack;
        b.l = 0;
        check(save_binding(file, b, &error), "saves");
        check(!std::filesystem::exists(dir / "pad.cfg.new"), "no temporary file is left");
        GamepadBinding loaded;
        check(load_binding(file, loaded, &error) == LoadResult::Loaded && same(loaded, b), "loads what was saved");
        const std::string good = read_all(file);
        std::ofstream(file, std::ios::binary | std::ios::trunc) << "a = 0x10zz\n";
        const std::string bad = read_all(file);
        check(load_binding(file, loaded, &error) == LoadResult::Invalid && !error.empty(), "a broken file is reported");
        check(same(loaded, GamepadBinding{}), "and the defaults are shown");
        check(read_all(file) == bad, "and the file is left as it was");
        std::filesystem::remove_all(dir, ec);
        check(!good.empty(), "the saved file had text");
    }
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS pad config\n");
    return 0;
}
