#pragma once

// The Audio, Controls, Gameplay and System categories of the settings registry (gekko/settings_registry.h).
//
// Two kinds of settings meet here:
//   * the PC's own (gekko/pc_settings.h, options.cfg): master volume, vibration strength, camera inversion;
//   * the game's own options: speaker output, vibration, text language - applied through the game's own apply on the
//     game thread (0x802B85D8; a language change through its fade and state 3) and persisted in options.cfg with the
//     PC settings (the user's decision of 2026-11-05, docs/SAVE_REROUTE_PLAN.md option C: not in the save or SRAM);
//     and the game's own commands: the sound test and Load Game Data. These are reached through GameOptions, which the
//     menu implements on the game thread; without it they are shown unavailable.
//
// Audio ("audio.*"):     master_volume  slider 0..100 step 5            (pc)
//                        music_volume, sfx_volume, voice_volume: sliders 0..100 step 5, each below the master (pc)
//                        speaker        stereo | monaural | dpl2         (game)
//                        voice          en | jp (jp only with the Japanese voice files, gekko/voice_language.h) (game)
//                        subtitles      jp | en | fr | sp | ge | it | hu: shown as "Text language" (the user's naming of
//                                       2026-11-05): all the game's text, subtitles included (game; enum order; id kept)
//                        subtitles_shown off | on: the spoken lines' subtitles drawn or not (pc)
//                        sound_test     action: the game's music list    (game; here until the front menu's Library)
// Controls ("controls.*"): vibration    off | on                         (game)
//                        rumble_strength "25" | "50" | "75" | "100"      (pc)
//                        invert_camera_x, invert_camera_y  off | on      (pc)
// Save Data ("gameplay.*", the registry's fifth category shown as the Save Data tile - the user's choice of
// 2026-11-05):           load_game      action: the game's Load          (game)
//                        autosave       off | on: settings + 0x7D        (game)
// System ("system.*"):   restore_all    action: every category's defaults (the menu runs it over the registry)
//                        build          one value: this build (information, not changeable)

#include <memory>
#include <string>
#include <string_view>

#include "gekko/settings_registry.h"

namespace gekko::settings {

class GameOptions {
public:
    virtual ~GameOptions() = default;
    virtual bool available() const = 0;  // the Options screen is running (the CPU can be reached)
    virtual unsigned speaker() const = 0;    // 0 stereo, 1 monaural, 2 Dolby Pro Logic II
    virtual bool vibration() const = 0;
    virtual unsigned language() const = 0;   // 0 JP, 1 EN, 2 FR, 3 SP, 4 GE, 5 IT
    virtual unsigned voice() const = 0;      // 0 Japanese, 1 English
    // Applies through the game's own apply (a language change through its fade and reload).
    virtual bool apply(unsigned speaker, bool vibration, unsigned language, unsigned voice, std::string* error) = 0;
    // The next apply must reload the front end's text even when the game's language value does not change (English
    // <-> Magyar: both the game's English slot, whose files now come out in the other language).
    virtual void reload_text_on_next_apply() {}
    // The game's autosave switch (its settings + 0x7D: the save after a stage and on leaving Options).
    virtual bool autosave() const = 0;
    virtual bool set_autosave(bool on, std::string* error) = 0;
    virtual bool run(std::string_view action, std::string* error) = 0;  // "audio.sound_test", "gameplay.load_game"
};
// The menu attaches its implementation (nullptr detaches).
void set_game_options(GameOptions* options) noexcept;

// Hungarian text (options.cfg language 6: the game on its English slot; the Hungarian subtitle work's
// native_hungarian_text serves every English text file as {English, Hungarian}, chosen at each read from options.cfg's
// language, so the switch is live). The mount sets it: `available` offers "Magyar" in the Text Language row, `mounted`
// says the switchable files are registered.
void set_hungarian_state(bool available, bool mounted) noexcept;
bool hungarian_mounted() noexcept;  // Hungarian is selected now (options.cfg language 6) and its files are registered

std::unique_ptr<Provider> make_audio_provider();
std::unique_ptr<Provider> make_controls_provider();
std::unique_ptr<Provider> make_gameplay_provider();
std::unique_ptr<Provider> make_system_provider(std::string build_info);

// Every category's provider added to the registry when it has none (the launcher may add its own first).
void add_default_providers(std::string build_info);

}  // namespace gekko::settings
