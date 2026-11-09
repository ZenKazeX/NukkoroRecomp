#pragma once

// The PC Options menu's own settings file, options.cfg beside display.cfg (its own file because display.cfg refuses
// unknown keys, so an older executable would refuse the whole file). Two groups:
//
//   PC settings (applied here, at once - each value is an atomic its owner reads):
//     master_volume    0..100 %  the endpoint gain after the game's mix (gekko/audio_runtime.h set_audio_output_gain)
//     music_volume     0..100 %  the music's level (the BGM ADX voices; gekko/audio_categories.h), 100 = untouched
//     sfx_volume       0..100 %  the sound effects' level (GCAX voices of the non-character banks)
//     voice_volume     0..100 %  the voices' level (dialogue / cutscene ADX voices and the character banks' GCAX voices)
//     rumble_strength  25..100 % the rumble motors' duty, of the original 0xC000 (gekko/xinput_pad.h)
//     invert_camera_x  bool      the camera stick's X axis inverted before the sample is latched (xinput_pad.h)
//     invert_camera_y  bool      the same for Y
//     subtitles        bool      show the subtitles of the spoken lines (default true = the original); false skips
//                                only their drawing (system.cpp, "Subtitles on/off")
//   The game's own options (the user's decision of 2026-11-05, docs/SAVE_REROUTE_PLAN.md option C: they live here,
//   not in the save or SRAM). The menu applies them through the game's own apply; the save path applies these values
//   over what the game reads at boot and after a load. -1 = not set (the first start fills them from the save):
//     language         0 JP, 1 EN, 2 FR, 3 SP, 4 GE, 5 IT (the game's enum)
//     speaker          0 stereo, 1 monaural, 2 Dolby Pro Logic II
//     vibration        0 off, 1 on
//     voice            0 Japanese, 1 English (the spoken language; Japanese needs the Japanese voice files)
//     autosave         0 off, 1 on (the game's autosave switch, settings + 0x7D)
//
// Defaults are the original behaviour. The file is read at startup; a bad value is reported and replaced by its
// default, unknown keys are reported and kept on save. save() rewrites only the changed keys, line-preserving
// (gekko/config_file.h). Every call is thread-safe.

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace gekko {

struct PcSettings {
    int master_volume = 100;
    int music_volume = 100;
    int sfx_volume = 100;
    int voice_volume = 100;
    int rumble_strength = 100;
    bool invert_camera_x = false;
    bool invert_camera_y = false;
    bool subtitles = true;
    int language = -1;   // the game's own options: -1 = not set
    int speaker = -1;
    int vibration = -1;
    int voice = -1;
    int autosave = -1;
};
bool operator==(const PcSettings& a, const PcSettings& b) noexcept;
inline bool operator!=(const PcSettings& a, const PcSettings& b) noexcept { return !(a == b); }

class PcSettingsService {
public:
    // Reads `path` (a missing file = the defaults) and applies the result. Problems go to `warnings`.
    void load(const std::filesystem::path& path, std::vector<std::string>* warnings = nullptr);
    bool loaded() const;
    std::filesystem::path path() const;

    PcSettings current() const;
    static bool validate(const PcSettings& settings, std::string* error);
    // Takes `settings` (false and `error` when validate() refuses it) and hands each value to its owner.
    bool apply(const PcSettings& settings, std::string* error = nullptr);
    // Writes the keys whose values differ from the file's (or the defaults when the file lacks them).
    bool save(std::string* error = nullptr);

private:
    void push(const PcSettings& settings) const;
    mutable std::mutex mutex_;
    PcSettings current_{}, saved_{};
    std::filesystem::path path_;
    bool loaded_ = false;
};

PcSettingsService& pc_settings();

}  // namespace gekko
