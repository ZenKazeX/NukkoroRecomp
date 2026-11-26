#pragma once

// The look of the game's two Windows programs, the Settings program (ShadowPadConfig.exe) and the Mod Manager
// (ShadowModManager.exe): their theme (colours), and their zoom (how large everything is). Both keep the choice in appearance.cfg
// beside them, so a choice made in one is used by the other the next time it opens. The game does not read the file.
//
//   theme = system      # system (the same as Windows' app mode), light, dark, or shadow (black with red)
//   zoom = 100          # percent of the normal size, 100 to 200
//
// This half has no window code and is unit tested (runtime/tests/ui_appearance_tests.cpp). The colours and the drawing
// of dark controls are in theme.h.

#include <filesystem>
#include <string>

namespace ui {

enum class Theme { System, Light, Dark, Shadow };

struct Appearance {
    Theme theme = Theme::System;
    int text_size = 100;  // the zoom, percent

    friend bool operator==(const Appearance& a, const Appearance& b) noexcept {
        return a.theme == b.theme && a.text_size == b.text_size;
    }
};

constexpr int kSmallestText = 100, kLargestText = 200, kTextStep = 25;
constexpr const char* kAppearanceFile = "appearance.cfg";

// The file's settings. A missing key, an unknown value or an unknown line leaves the default; a text size outside
// 100..200 is moved to the nearest end.
Appearance parse_appearance(const std::string& text);

// `text` with these settings: each theme / zoom line rewritten, every other line, comment and line ending kept,
// a missing key added at the end. An empty `text` gives a new file with a comment saying what it is.
std::string update_appearance(const std::string& text, const Appearance& appearance);

// `now` moved by `steps` sizes of kTextStep, kept within kSmallestText..kLargestText. 0 steps snaps to a size.
int step_text_size(int now, int steps);

// The settings in folder/appearance.cfg; the defaults when there is none or it cannot be read.
Appearance load_appearance(const std::filesystem::path& folder);

// Writes folder/appearance.cfg through a temporary file, keeping its other lines. Nothing is written when the file
// already says the same. False when it could not be written.
bool save_appearance(const std::filesystem::path& folder, const Appearance& appearance);

}  // namespace ui
