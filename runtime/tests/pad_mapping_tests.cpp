// Tests for parse_gamepad_binding (the lifted peripheral layer,
// docs/lift-20261006/L1_INPUT.md): the contract between tools/launcher.py, which
// writes pad.cfg, and the runtime, which reads it. The file format is unchanged.
//
// What this suite holds:
//   1. The exact text the launcher writes - aligned keys, hex masks, trailing
//      "# A -> GC A" comments - loads, and loads to the values it names.
//   2. A value that is not wholly a number is an error, not a prefix: "0x10zz"
//      must not load as 0x10.
//   3. A value out of range for its field is an error, not a wrap: a narrowing
//      cast would turn trigger_threshold 300 into 44 and -5 into 251, a setting
//      that loads without complaint and behaves like a different one.
//   4. An unknown key is an error, so a typo cannot silently keep a default.

#include "peripheral/gamepad_binding.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

bool parses(const std::string& text, shadow::peripheral::GamepadBinding& out, std::string* error = nullptr) {
    std::string local;
    return shadow::peripheral::parse_gamepad_binding(text, out, error ? error : &local);
}

// Byte-for-byte what tools/launcher.py pad_save() produces for a remapped pad.
const char* kLauncherFile =
    "# GekkoForge host input binding. Written by tools/launcher.py.\n"
    "# Values are XInput button masks; 0 means unbound.\n"
    "\n"
    "a            = 0x2000   # B -> GC A\n"
    "b            = 0x1000   # A -> GC B\n"
    "x            = 0x4000   # X -> GC X\n"
    "y            = 0x8000   # Y -> GC Y\n"
    "start        = 0x0010   # START -> GC Start\n"
    "l            = 0x0100   # LEFT_SHOULDER -> GC L\n"
    "r            = 0x0000   # unbound -> GC R\n"
    "z            = 0x0200   # RIGHT_SHOULDER -> GC Z\n"
    "dpad_up      = 0x0001   # DPAD_UP -> GC D-Pad Up\n"
    "dpad_down    = 0x0002   # DPAD_DOWN -> GC D-Pad Down\n"
    "dpad_left    = 0x0004   # DPAD_LEFT -> GC D-Pad Left\n"
    "dpad_right   = 0x0008   # DPAD_RIGHT -> GC D-Pad Right\n"
    "\n"
    "trigger_threshold = 64\n"
    "swap_sticks       = 1\n";

void launcher_file_round_trips() {
    shadow::peripheral::GamepadBinding m;
    std::string error;
    check(parses(kLauncherFile, m, &error), "launcher-written pad.cfg loads: " + error);
    check(m.a == 0x2000 && m.b == 0x1000, "A/B swap is applied");
    check(m.l == 0x0100 && m.r == 0 && m.z == 0x0200, "l/r/z load");
    check(m.right == 0x0008 && m.up == 0x0001, "dpad masks load");
    check(m.trigger_threshold == 64, "threshold loads");
    check(m.swap_sticks, "swap_sticks loads");
}

void partial_numbers_are_errors() {
    shadow::peripheral::GamepadBinding m;
    check(!parses("a = 0x10zz\n", m), "a trailing garbage value is rejected, not read as 0x10");
    check(!parses("trigger_threshold = 40abc\n", m), "a trailing-garbage threshold is rejected");
    check(!parses("a = yes\n", m), "a non-number is rejected");
    check(parses("a = 0x1000   \n", m) && m.a == 0x1000, "trailing whitespace is still accepted");
}

void out_of_range_is_an_error() {
    shadow::peripheral::GamepadBinding m;
    std::string error;
    check(!parses("trigger_threshold = 300\n", m, &error), "threshold 300 is rejected, not wrapped to 44");
    check(error.find("0 to 255") != std::string::npos, "the error names the valid range");
    check(!parses("trigger_threshold = -5\n", m), "a negative threshold is rejected, not wrapped to 251");
    check(parses("trigger_threshold = 255\n", m) && m.trigger_threshold == 255, "255 is the upper bound");
    check(parses("trigger_threshold = 0\n", m) && m.trigger_threshold == 0, "0 is the lower bound");
    check(!parses("a = 0x10000\n", m), "a mask wider than 16 bits is rejected");
    check(parses("a = 0xFFFF\n", m) && m.a == 0xFFFF, "0xFFFF is the widest mask");
}

void unknown_keys_are_errors() {
    shadow::peripheral::GamepadBinding m;
    std::string error;
    check(!parses("trigger_l = 0x0100\n", m, &error), "the struct field name is not a file key");
    check(error.find("unknown key") != std::string::npos, "the error says the key is unknown");
}

}  // namespace

int main() {
    launcher_file_round_trips();
    partial_numbers_are_errors();
    out_of_range_is_an_error();
    unknown_keys_are_errors();
    if (g_failures == 0) {
        std::printf("pad_mapping_tests: all passed\n");
        return 0;
    }
    std::printf("pad_mapping_tests: %d failure(s)\n", g_failures);
    return 1;
}
