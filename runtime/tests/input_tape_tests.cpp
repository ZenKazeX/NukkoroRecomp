// Tests of the gameplay recorder's tape (gekko/input_tape.h, docs/tools/RECORDER.md): a version 3 tape round-trips its
// meta, logic, settings, checkpoints and samples; versions 1 and 2 still read (a version 2 output is its logic aspect);
// a checkpoint names the groups that differ (gekko/replay_check.h).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "gekko/input_tape.h"
#include "gekko/replay_check.h"
#include "test_scratch.h"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

std::filesystem::path temp(const char* name) { return gekko::test_support::scratch_directory(name); }

void round_trip() {
    gekko::InputTape tape;
    tape.begin_recording(1000, 2560, 1440);
    tape.set_meta("boot_stage", "602");
    tape.set_meta("display", "mode windowed resolution 2560x1440 aspect 0 render_scale 100 msaa 8 anisotropy 16");
    tape.note_output(1000, 3840, 1200);
    const float wide = 3840.0f / 1200.0f;
    tape.note_logic(1000, wide);
    tape.note_logic(1000 + 202500, wide);  // a repeat: dropped
    tape.note_logic(5000000, 16.0f / 9.0f);
    tape.note_setting(4000000, "draw_distance", "2");
    tape.note_check(41500000, "stage=00000001 rng=00000002 player=00000003 mission=00000004 camera=00000005");
    gekko::InputTape::Samples pads{};
    pads[0].connected = true;
    pads[0].controls = 0x0100;
    pads[0].stick_x = 50;
    tape.observe(1000, pads);
    pads[0].stick_x = -20;
    tape.observe(1000 + 202500, pads);
    const auto path = temp("input_tape_tests_v3.tape");
    std::string error;
    check(tape.write(path, 0, 0, &error), "a version 3 tape is written");

    gekko::InputTape back;
    check(back.load(path, &error), "and loads");
    check(back.version() == 3, "as version 3");
    check(back.meta("boot_stage") == "602", "meta: the start route");
    check(back.meta("display").find("msaa 8") != std::string::npos, "meta: the display settings, spaces kept");
    float logic = 0.0f;
    check(back.logic_at(1000, &logic) && logic == wide, "the first logic aspect, exact (binary32 round trip)");
    check(!back.logic_at(4999999, &logic), "no logic change before its tick");
    check(back.logic_at(5000000, &logic) && logic == 16.0f / 9.0f, "the logic change at its tick");
    const auto due = back.settings_at(4000000);
    check(due.size() == 1u && due[0].key == "draw_distance" && due[0].value == "2", "a setting at its tick");
    std::string text;
    check(!back.check_at(41000000, &text), "no checkpoint before its tick");
    check(back.check_at(41500000, &text) && text.rfind("stage=00000001", 0) == 0, "the checkpoint at its tick");
    gekko::InputTape::Samples latched{};
    back.samples_at(1000, latched);
    check(latched[0].connected && latched[0].stick_x == 50, "samples replay");
    std::filesystem::remove(path);
}

void older_versions() {
    const auto path = temp("input_tape_tests_v2.tape");
    {
        std::ofstream out(path, std::ios::binary);
        out << "shadow-input-tape 2\nstart 0\ndisplay 3840x1200\noutput 0 3840x1200\n"
            << "sample 0 0 1 0000 0 0 0 0\nend\n";
    }
    gekko::InputTape v2;
    std::string error;
    check(v2.load(path, &error), "a version 2 tape loads");
    check(v2.version() == 2 && v2.has_outputs() && v2.has_logic(), "its output is its logic");
    float logic = 0.0f;
    check(v2.logic_at(0, &logic) && logic == static_cast<float>(3840) / static_cast<float>(1200),
          "the logic aspect is the output's w/h as the game computed it");
    check(v2.check_count() == 0u, "and it has no checkpoints");
    {
        std::ofstream out(path, std::ios::binary);
        out << "shadow-input-tape 1\nstart 0\ndisplay 1280x720\nsample 0 0 1 0000 0 0 0 0\nend\n";
    }
    gekko::InputTape v1;
    check(v1.load(path, &error) && v1.version() == 1 && !v1.has_logic(), "a version 1 tape loads, without logic");
    {
        std::ofstream out(path, std::ios::binary);
        out << "shadow-input-tape 3\nstart 0\ndisplay 1280x720\nsample 0 0 1 0000 0 0 0 0\n";
    }
    gekko::InputTape cut;
    check(!cut.load(path, &error), "a tape without its end line is refused (truncated)");
    std::filesystem::remove(path);
}

void checkpoints() {
    const std::string a = "stage=00000001 rng=00000002 player=00000003 mission=00000004 camera=00000005";
    const std::string b = "stage=00000001 rng=00000009 player=00000003 mission=00000004 camera=0000000a";
    check(gekko::replay_check::differing_groups(a, a).empty(), "equal checkpoints: nothing differs");
    check(gekko::replay_check::differing_groups(a, b) == "rng,camera", "the differing groups are named");
    // compute() over a fake memory: deterministic, and a change in the player's record changes only that group.
    std::uint32_t player_word = 7;
    const auto memory = [&](std::uint32_t address) -> std::uint32_t {
        if (address == 0x805777A4u) return player_word;
        return address * 2654435761u;  // not a guest pointer in general: the pointer paths take their marker
    };
    const std::string first = gekko::replay_check::compute(memory);
    check(first == gekko::replay_check::compute(memory), "compute() is deterministic");
    player_word = 8;
    check(gekko::replay_check::differing_groups(first, gekko::replay_check::compute(memory)) == "player",
          "a player record change shows in the player group only");
}

}  // namespace

int main() {
    round_trip();
    older_versions();
    checkpoints();
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("input_tape_tests: all passed\n");
    return 0;
}
