#pragma once

// The settings program's files without any window: display.cfg, options.cfg and controls.cfg as the game reads them
// (docs/PC_SETTINGS_FILES.md, docs/controls/KEYBOARD_MOUSE_BINDINGS.md). pad.cfg is pad_config.h.
//
// Reading follows the game's own loaders key by key (gekko/display.h parse_display_file, gekko/pc_settings.h
// PcSettingsService::load, peripheral/action_map.h parse_controls). Writing goes through the game's line-preserving
// writer (gekko/config_file.h): only the keys whose value the player changed are written, every other line, comment and
// line ending stays, keys the file lacks are appended, and nothing is created or written when nothing changed. What is
// about to be written is read back first; anything the game would not read back as the same settings is refused.

#include <filesystem>
#include <string>
#include <vector>

#include "gekko/config_file.h"
#include "peripheral/action_map.h"

namespace padconfig {

// ---- display.cfg ------------------------------------------------------------------------------------------------
// The defaults are the game's when a key is absent (gekko/display_settings.h DisplaySettings, the launcher's flags).
struct DisplayPrefs {
    bool fullscreen = false;
    bool exclusive = false;  // with fullscreen: the monitor's own mode fullscreen_width x fullscreen_height
    int width = 1280;
    int height = 720;
    int fullscreen_width = 0;  // 0 x 0 = the desktop's mode
    int fullscreen_height = 0;
    double aspect = 0.0;  // 0 = auto
    double render_scale = 100.0;
    int msaa = 4;         // 1 = off
    int anisotropy = 16;  // 1 = off
    double render_rate = 60.0;  // 0 = unlocked
    bool vsync = true;
    bool frame_interpolation = false;
    double draw_distance = 1.0;
    bool fps_counter = false;

    friend bool operator==(const DisplayPrefs&, const DisplayPrefs&) = default;
};

// The values the game would use for `text`. False (and `error`, worded as the game words it) when the game would stop
// at start on this file; `out` then holds the defaults. `warnings` gets the lines the game accepts with a warning.
bool read_display(const std::string& text, DisplayPrefs& out, std::string* error,
                  std::vector<std::string>* warnings = nullptr);
// A plain-English reason when the game would refuse to use these values, else empty. The window and the exclusive mode
// at least 320 x 240 (gekko/display_geometry.h), scale 25..200, render rate unlocked or (0, 1000], draw distance 1..8.
std::string check_display(const DisplayPrefs& prefs);
// `text` with the values of `after` that differ from `before` written.
std::string write_display(const std::string& text, const DisplayPrefs& before, const DisplayPrefs& after);

// ---- options.cfg ------------------------------------------------------------------------------------------------
// The game's own options (language, speaker, vibration, voice, autosave) are -1 when the file does not set them: the
// game then takes them from the save and writes them here.
struct OptionPrefs {
    int master_volume = 100;
    int music_volume = 100;
    int sfx_volume = 100;
    int voice_volume = 100;
    int rumble_strength = 100;
    bool invert_camera_x = false;
    bool invert_camera_y = false;
    bool subtitles = true;
    int language = -1;  // 0 Japanese, 1 English, 2 French, 3 Spanish, 4 German, 5 Italian
    int speaker = -1;   // 0 stereo, 1 mono, 2 Dolby Pro Logic II
    int vibration = -1; // 0 off, 1 on
    int voice = -1;     // 0 Japanese, 1 English
    int autosave = -1;  // 0 off, 1 on

    friend bool operator==(const OptionPrefs&, const OptionPrefs&) = default;
};

// The values the game would use: a bad value keeps its default (reported in `warnings`), as the game does.
void read_options(const std::string& text, OptionPrefs& out, std::vector<std::string>* warnings = nullptr);
std::string check_options(const OptionPrefs& prefs);
// A game option changed back to -1 has its line removed (the game then decides again).
std::string write_options(const std::string& text, const OptionPrefs& before, const OptionPrefs& after);

// ---- controls.cfg -----------------------------------------------------------------------------------------------
using shadow::peripheral::ControlsConfig;
// The default layout with what `text` sets over it; a line the game would skip is skipped and reported.
void read_controls(const std::string& text, ControlsConfig& out, std::vector<std::string>* warnings = nullptr);
std::string check_controls(const ControlsConfig& config);
std::string write_controls(const std::string& text, const ControlsConfig& before, const ControlsConfig& after);

// ---- saving -----------------------------------------------------------------------------------------------------
// Writes to the file at `path` the settings the player changed (those that differ between `before`, what was loaded,
// and `after`). The file is read again first, so a key the game wrote meanwhile (a window resized, Alt+Enter) is kept.
// Nothing is written (and no file is created) when nothing changed. False and a plain-English `error` when the values
// are refused, the file as it is now would stop the game (display.cfg), or the written text would not read back as
// intended; the file is then left as it was.
bool save_display(const std::filesystem::path& path, const DisplayPrefs& before, const DisplayPrefs& after,
                  std::string* error);
bool save_options(const std::filesystem::path& path, const OptionPrefs& before, const OptionPrefs& after,
                  std::string* error);
bool save_controls(const std::filesystem::path& path, const ControlsConfig& before, const ControlsConfig& after,
                   std::string* error);

// ---- shared -----------------------------------------------------------------------------------------------------
// `text` without the lines whose key (case-insensitive) is one of `keys` (lower case). Other bytes stay.
std::string remove_keys(const std::string& text, const std::vector<std::string>& keys);
// The shortest spelling of a number that reads back the same ("60", "59.94").
std::string format_number(double value);

// Reads a whole file; a missing file is empty text and `exists` false. False when it exists and cannot be read.
bool read_file(const std::filesystem::path& path, std::string& text, bool* exists, std::string* error);
// Writes beside the target and renames over it, so a failed write leaves the old file whole.
bool write_file(const std::filesystem::path& path, const std::string& text, std::string* error);

}  // namespace padconfig
