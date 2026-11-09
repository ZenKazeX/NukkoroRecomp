// The Audio, Controls, Gameplay and System categories. Contract: include/gekko/settings_game.h.

#include "gekko/settings_game.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <optional>

#include "gekko/pc_settings.h"
#include "gekko/settings_display.h"
#include "gekko/voice_language.h"

namespace gekko::settings {

namespace {

std::atomic<GameOptions*> g_game{nullptr};
// Hungarian text (language 6): whether its tables can be served, and whether they were served at this mount.
std::atomic<bool> g_hungarian_available{false}, g_hungarian_mounted{false};

GameOptions* game() {
    GameOptions* g = g_game.load(std::memory_order_acquire);
    return g != nullptr && g->available() ? g : nullptr;
}

const std::vector<ChoiceView> kOffOn = {{"off", "OFF"}, {"on", "ON"}};
constexpr int kRumbleLevels[] = {25, 50, 75, 100};

// The game's own options, pending: what the page shows and apply() hands to GameOptions.
struct GameValues {
    unsigned speaker = 0;
    bool vibration = true;
    unsigned language = 1;
    unsigned voice = 1;  // 0 Japanese, 1 English (the game's enum)
    bool autosave = true;
    bool operator==(const GameValues& o) const {
        return speaker == o.speaker && vibration == o.vibration && language == o.language && voice == o.voice &&
               autosave == o.autosave;
    }
};

// The options handed to the game's apply whose state 3 (fade, 0x802B85D8) has not written them to the settings object
// yet. Several categories can apply in one go (Restore all defaults): each must build on what the earlier ones asked
// for, or the later apply would hand the game the old value of a field the earlier one changed. Providers run under
// the registry's lock, so no lock of its own.
std::optional<GameValues> g_inflight;

GameValues read_game() {
    GameValues v;
    if (GameOptions* g = game()) {
        v.speaker = std::min(g->speaker(), 2u);
        v.vibration = g->vibration();
        v.language = std::min(g->language(), 5u);
        // Hungarian (language 6, the Hungarian subtitle work) runs the game on its English text slot with Hungarian
        // tables served at mount: the game reports 1, options.cfg holds 6.
        if (v.language == 1u && pc_settings().current().language == 6) v.language = 6u;
        v.voice = std::min(g->voice(), 1u);
        v.autosave = g->autosave();
        if (g_inflight.has_value()) {
            const GameValues& in = *g_inflight;
            if (in.speaker == v.speaker && in.vibration == v.vibration && in.language == v.language &&
                in.voice == v.voice) {
                g_inflight.reset();  // the game has caught up
            } else {
                v.speaker = in.speaker;
                v.vibration = in.vibration;
                v.language = in.language;
                v.voice = in.voice;
            }
        }
    }
    return v;
}

// The rumble level (kRumbleLevels index) nearest to an options.cfg value: the file accepts 25..100, the page offers
// four steps, and the page must show the step the value is closest to rather than silently 100 %.
int nearest_rumble_level(int percent) {
    int best = 3;
    for (int i = 0; i < 4; ++i)
        if (std::abs(kRumbleLevels[i] - percent) < std::abs(kRumbleLevels[best] - percent)) best = i;
    return best;
}

// The game's language names (its Subtitle page's values), in its enum order.
const std::vector<ChoiceView> kLanguages = {{"jp", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"}, {"en", "English"},
                                            {"fr", "Fran\xC3\xA7" "ais"},  {"sp", "Espa\xC3\xB1ol"},
                                            {"ge", "Deutsch"},             {"it", "Italiano"},
                                            {"hu", "Magyar"}};  // 6: Hungarian text on the game's English slot

SettingView choice(const char* id, const char* label, const char* help, std::vector<ChoiceView> choices, int applied,
                   int pending, Kind kind = Kind::Choice) {
    SettingView v;
    v.id = id;
    v.kind = kind;
    v.english_label = label;
    v.english_help = help;
    v.choices = std::move(choices);
    v.applied = applied;
    v.pending = pending;
    return v;
}

SettingView unavailable_if_no_game(SettingView v) {
    if (game() == nullptr) {
        v.available = false;
        v.reason = "only while the Options screen runs";
    }
    return v;
}

// The shared part: PC settings and game values, pending against in effect.
class GameProvider : public Provider {
public:
    void begin() override {
        pc_ = pc_applied_ = pc_settings().current();
        game_ = game_applied_ = read_game();
    }
    bool dirty() const override { return pc_ != pc_applied_ || !(game_ == game_applied_); }
    void discard() override {
        pc_ = pc_applied_;
        game_ = game_applied_;
    }
    bool set(std::string_view id, int value) override {
        const std::vector<SettingView> list = views();
        for (const SettingView& v : list) {
            if (v.id != id || !v.available) continue;
            if (v.kind == Kind::Action || (v.choices.empty() && v.kind != Kind::Slider)) return false;  // nothing to set
            const int now = v.pending;
            const int want = v.kind == Kind::Slider ? std::clamp(value, v.slider_min, v.slider_max)
                                                    : std::clamp(value, 0, static_cast<int>(v.choices.size()) - 1);
            if (want == now) return false;
            write(id, want);
            return true;
        }
        return false;
    }
    bool step(std::string_view id, int delta) override {
        for (const SettingView& v : views()) {
            if (v.id != id) continue;
            if (v.kind == Kind::Action) return false;
            return set(id, v.pending + delta * (v.kind == Kind::Slider ? v.slider_step : 1));
        }
        return false;
    }
    ApplyResult apply(std::string* error) override {
        if (!dirty()) return ApplyResult::Nothing;
        if (pc_ != pc_applied_) {
            // Only the PC-owned values this page holds: its copy of the game-owned ones (and of the other pages' PC
            // values) may be older than what is in effect now.
            PcSettings p = pc_settings().current();
            if (pc_.master_volume != pc_applied_.master_volume) p.master_volume = pc_.master_volume;
            if (pc_.music_volume != pc_applied_.music_volume) p.music_volume = pc_.music_volume;
            if (pc_.sfx_volume != pc_applied_.sfx_volume) p.sfx_volume = pc_.sfx_volume;
            if (pc_.voice_volume != pc_applied_.voice_volume) p.voice_volume = pc_.voice_volume;
            if (pc_.rumble_strength != pc_applied_.rumble_strength) p.rumble_strength = pc_.rumble_strength;
            if (pc_.invert_camera_x != pc_applied_.invert_camera_x) p.invert_camera_x = pc_.invert_camera_x;
            if (pc_.invert_camera_y != pc_applied_.invert_camera_y) p.invert_camera_y = pc_.invert_camera_y;
            if (pc_.subtitles != pc_applied_.subtitles) p.subtitles = pc_.subtitles;
            if (!pc_settings().apply(p, error)) return ApplyResult::Failed;
            pc_applied_ = pc_ = p;
        }
        if (!(game_ == game_applied_)) {
            GameOptions* g = game();
            if (g == nullptr) {
                if (error != nullptr) *error = "the game's options can only change on the Options screen";
                return ApplyResult::Failed;
            }
            // Merge: only the fields this page changed go over what is in effect now (with what earlier applies of
            // this go asked for), so a page whose copy is older cannot take another page's change back.
            const GameValues now = read_game();
            GameValues target = now;
            if (game_.speaker != game_applied_.speaker) target.speaker = game_.speaker;
            if (game_.vibration != game_applied_.vibration) target.vibration = game_.vibration;
            if (game_.language != game_applied_.language) target.language = game_.language;
            if (game_.voice != game_applied_.voice) target.voice = game_.voice;
            if (game_.autosave != game_applied_.autosave) {
                if (!g->set_autosave(game_.autosave, error)) return ApplyResult::Failed;
                target.autosave = game_.autosave;
            }
            const bool options = target.speaker != now.speaker || target.vibration != now.vibration ||
                                 target.language != now.language || target.voice != now.voice;
            if (options) {
                // Hungarian (6) is the game's English slot (1) with Hungarian tables: the game gets 1, options.cfg 6.
                const unsigned game_language = target.language == 6u ? 1u : target.language;
                const unsigned game_language_now = now.language == 6u ? 1u : now.language;
                const bool crosses_hungarian = (target.language == 6u) != (now.language == 6u);
                // The text files choose English or Hungarian from options.cfg's language at each read: it must say
                // the new language before the game's apply re-reads them.
                const int language_before = pc_settings().current().language;
                if (crosses_hungarian) {
                    PcSettings p = pc_settings().current();
                    p.language = static_cast<int>(target.language);
                    if (!pc_settings().apply(p, error)) return ApplyResult::Failed;
                    // English <-> Magyar: the game's value stays 1 and its apply would skip the reload.
                    if (game_language == game_language_now) g->reload_text_on_next_apply();
                }
                if (!g->apply(target.speaker, target.vibration, game_language, target.voice, error)) {
                    if (crosses_hungarian) {
                        PcSettings p = pc_settings().current();
                        p.language = language_before;
                        pc_settings().apply(p, nullptr);
                    }
                    return ApplyResult::Failed;
                }
                g_inflight = target;
                std::cerr << "[options] game apply: speaker " << target.speaker << ", vibration "
                          << (target.vibration ? 1 : 0) << ", language " << target.language << ", voice "
                          << target.voice << ", autosave " << (target.autosave ? 1 : 0) << '\n';
            }
            game_ = game_applied_ = target;
            // The game's options persist in options.cfg (docs/SAVE_REROUTE_PLAN.md, option C), not in the save.
            PcSettings p = pc_settings().current();
            p.language = static_cast<int>(target.language);
            p.speaker = static_cast<int>(target.speaker);
            p.vibration = target.vibration ? 1 : 0;
            p.voice = static_cast<int>(target.voice);
            p.autosave = target.autosave ? 1 : 0;
            if (!pc_settings().apply(p, error)) return ApplyResult::Failed;
            pc_ = pc_applied_ = p;
        }
        return ApplyResult::Applied;
    }
    // Both parts persist in options.cfg (the game's options too: docs/SAVE_REROUTE_PLAN.md, option C).
    bool save(std::string* error) override { return pc_settings().save(error); }
    bool run(std::string_view id, std::string* error) override {
        GameOptions* g = game();
        if (g == nullptr) {
            if (error != nullptr) *error = "only while the Options screen runs";
            return false;
        }
        return g->run(id, error);
    }

protected:
    virtual void write(std::string_view id, int value) = 0;
    PcSettings pc_{}, pc_applied_{};
    GameValues game_{}, game_applied_{};
};

class AudioProvider final : public GameProvider {
public:
    Category category() const override { return Category::Audio; }
    std::vector<SettingView> views() const override {
        std::vector<SettingView> out;
        SettingView volume = choice("audio.master_volume", "Master volume", "Adjust the overall volume of the game.",
                                    {}, pc_applied_.master_volume, pc_.master_volume, Kind::Slider);
        volume.slider_min = 0;
        volume.slider_max = 100;
        volume.slider_step = 5;
        out.push_back(volume);
        // The per-category levels (gekko/audio_categories.h): music, sound effects and voices, each on top of the master.
        const auto category = [&](const char* id, const char* label, const char* help, int applied, int pending) {
            SettingView v = choice(id, label, help, {}, applied, pending, Kind::Slider);
            v.slider_min = 0;
            v.slider_max = 100;
            v.slider_step = 5;
            out.push_back(v);
        };
        category("audio.music_volume", "Music volume", "Adjust the volume of the music.", pc_applied_.music_volume,
                 pc_.music_volume);
        category("audio.sfx_volume", "Sound effects volume", "Adjust the volume of the sound effects.",
                 pc_applied_.sfx_volume, pc_.sfx_volume);
        category("audio.voice_volume", "Voice volume", "Adjust the volume of the characters' voices.",
                 pc_applied_.voice_volume, pc_.voice_volume);
        out.push_back(unavailable_if_no_game(choice(
            "audio.speaker", "Speaker output", "Change the speaker output settings.",
            {{"stereo", "Stereo"}, {"monaural", "Monaural"}, {"dpl2", "Dolby Pro Logic II"}},
            static_cast<int>(game_applied_.speaker), static_cast<int>(game_.speaker))));
        // The spoken language (the game's hidden voice-language item): Japanese needs the Japanese voice files
        // (gekko/voice_language.h); without them the row shows English and cannot change.
        {
            const int applied = game_applied_.voice == 0u ? 1 : 0, pending = game_.voice == 0u ? 1 : 0;
            SettingView voice = unavailable_if_no_game(
                choice("audio.voice", "Voice", "Change the language the characters speak.",
                       {{"en", "English"}, {"jp", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"}}, applied, pending));
            if (voice.available && !japanese_voices_available()) {
                voice.available = false;
                voice.reason = "the Japanese voice files are not installed";
            }
            out.push_back(voice);
        }
        // The text and subtitle language (the game's Subtitle item): every text of the game, its menus included.
        {
            // Magyar (6) is offered only when the Hungarian tables can be served (set_hungarian_available).
            std::vector<ChoiceView> languages = kLanguages;
            if (!g_hungarian_available.load(std::memory_order_relaxed) && game_applied_.language != 6u &&
                game_.language != 6u)
                languages.pop_back();
            SettingView subtitles = unavailable_if_no_game(
                choice("audio.subtitles", "Text Language", "Change the language of all the game's text,\nsubtitles included.",
                       std::move(languages), static_cast<int>(game_applied_.language),
                       static_cast<int>(game_.language)));
            // Magyar switches live (the text files follow options.cfg's language at each read): no restart.
            out.push_back(subtitles);
        }
        // Show or hide the subtitles of the spoken lines (a PC switch; the original has none): the text drawing of
        // the voice-line messages is skipped, nothing else (system.cpp, "Subtitles on/off").
        out.push_back(choice("audio.subtitles_shown", "Show subtitles",
                             "Show or hide the subtitles of the spoken\nlines.", kOffOn,
                             pc_applied_.subtitles ? 1 : 0, pc_.subtitles ? 1 : 0, Kind::Toggle));
        // The game's own music player stays here (its original home, the Options screen) until the front menu's
        // Library entry takes it (the user's plan of 2026-11-05).
        out.push_back(unavailable_if_no_game(
            choice("audio.sound_test", "Sound test", "Listen to the in-game music.", {}, 0, 0, Kind::Action)));
        return out;
    }
    void reset_defaults() override {
        pc_.master_volume = PcSettings{}.master_volume;
        pc_.music_volume = PcSettings{}.music_volume;
        pc_.sfx_volume = PcSettings{}.sfx_volume;
        pc_.voice_volume = PcSettings{}.voice_volume;
        pc_.subtitles = PcSettings{}.subtitles;
        game_.speaker = 0u;
    }

private:
    void write(std::string_view id, int value) override {
        // The volume sliders take effect as they move (the user's request of 2026-11-05: "realtime volume sliders"):
        // each step is applied and saved at once - heard live, nothing left pending - like every PC game's sliders.
        const auto live = [&](int PcSettings::* field) {
            pc_.*field = value;
            PcSettings p = pc_settings().current();
            p.*field = value;
            if (pc_settings().apply(p, nullptr)) {
                pc_applied_.*field = value;
                pc_settings().save(nullptr);
            }
        };
        if (id == "audio.master_volume") live(&PcSettings::master_volume);
        else if (id == "audio.music_volume") live(&PcSettings::music_volume);
        else if (id == "audio.sfx_volume") live(&PcSettings::sfx_volume);
        else if (id == "audio.voice_volume") live(&PcSettings::voice_volume);
        else if (id == "audio.speaker") game_.speaker = static_cast<unsigned>(value);
        else if (id == "audio.voice") game_.voice = value == 1 ? 0u : 1u;  // choices: 0 English, 1 Japanese
        else if (id == "audio.subtitles") game_.language = static_cast<unsigned>(value);
        else if (id == "audio.subtitles_shown") pc_.subtitles = value != 0;
    }
};

class ControlsProvider final : public GameProvider {
public:
    Category category() const override { return Category::Controls; }
    std::vector<SettingView> views() const override {
        std::vector<SettingView> out;
        out.push_back(unavailable_if_no_game(choice("controls.vibration", "Vibration",
                                                    "Turn the controller vibration on or off.", kOffOn,
                                                    game_applied_.vibration ? 1 : 0, game_.vibration ? 1 : 0,
                                                    Kind::Toggle)));
        std::vector<ChoiceView> levels;
        int applied = 3, pending = 3;
        for (int i = 0; i < 4; ++i) {
            levels.push_back({std::to_string(kRumbleLevels[i]), std::to_string(kRumbleLevels[i]) + "%"});
        }
        applied = nearest_rumble_level(pc_applied_.rumble_strength);
        pending = nearest_rumble_level(pc_.rumble_strength);
        out.push_back(choice("controls.rumble_strength", "Vibration strength",
                             "Adjust how strongly the controller vibrates.", levels, applied, pending));
        out.push_back(choice("controls.invert_camera_x", "Invert camera X",
                             "Reverse the left and right direction of the\ncamera stick.", kOffOn,
                             pc_applied_.invert_camera_x ? 1 : 0, pc_.invert_camera_x ? 1 : 0, Kind::Toggle));
        out.push_back(choice("controls.invert_camera_y", "Invert camera Y",
                             "Reverse the up and down direction of the\ncamera stick.", kOffOn,
                             pc_applied_.invert_camera_y ? 1 : 0, pc_.invert_camera_y ? 1 : 0, Kind::Toggle));
        return out;
    }
    void reset_defaults() override {
        pc_.rumble_strength = PcSettings{}.rumble_strength;
        pc_.invert_camera_x = pc_.invert_camera_y = false;
        game_.vibration = true;
    }

private:
    void write(std::string_view id, int value) override {
        if (id == "controls.vibration") game_.vibration = value != 0;
        else if (id == "controls.rumble_strength") pc_.rumble_strength = kRumbleLevels[std::clamp(value, 0, 3)];
        else if (id == "controls.invert_camera_x") pc_.invert_camera_x = value != 0;
        else if (id == "controls.invert_camera_y") pc_.invert_camera_y = value != 0;
    }
};

// The registry's fifth category (its ids keep the "gameplay." prefix) is the menu's Save Data tile (the user's choice
// of 2026-11-05): the game's own Load (moved from System) and its autosave switch.
class SaveDataProvider final : public GameProvider {
public:
    Category category() const override { return Category::Gameplay; }
    std::vector<SettingView> views() const override {
        return {unavailable_if_no_game(
                    choice("gameplay.load_game", "Load Game Data",
                           "Load Game Data. Your progress and any\nchanges made will be saved automatically.", {}, 0,
                           0, Kind::Action)),
                unavailable_if_no_game(choice("gameplay.autosave", "Autosave",
                                              "Turn the game's automatic saving on or\noff.", kOffOn,
                                              game_applied_.autosave ? 1 : 0, game_.autosave ? 1 : 0, Kind::Toggle))};
    }
    void reset_defaults() override { game_.autosave = true; }

private:
    void write(std::string_view id, int value) override {
        if (id == "gameplay.autosave") game_.autosave = value != 0;
    }
};

class SystemProvider final : public GameProvider {
public:
    explicit SystemProvider(std::string build) : build_(std::move(build)) {}
    Category category() const override { return Category::System; }
    std::vector<SettingView> views() const override {
        std::vector<SettingView> out;
        out.push_back(choice("system.restore_all", "Restore all defaults",
                             "Return every setting of this menu to its\ndefault, except the languages.", {}, 0, 0,
                             Kind::Action));
        SettingView build = choice("system.build", "Build", "The build of this PC version.", {{"build", build_}}, 0, 0);
        build.available = false;  // information: shown, never changed
        build.reason = "information";
        out.push_back(build);
        return out;
    }
    void reset_defaults() override {}
    bool run(std::string_view id, std::string* error) override {
        if (id == "system.restore_all") {
            if (error != nullptr) *error = "restore_all is run by the menu over every category";
            return false;
        }
        return GameProvider::run(id, error);
    }

private:
    void write(std::string_view, int) override {}
    std::string build_;
};

}  // namespace

void set_game_options(GameOptions* options) noexcept { g_game.store(options, std::memory_order_release); }

void set_hungarian_state(bool available, bool mounted) noexcept {
    g_hungarian_available.store(available, std::memory_order_relaxed);
    g_hungarian_mounted.store(mounted, std::memory_order_relaxed);
}

bool hungarian_mounted() noexcept {
    return g_hungarian_mounted.load(std::memory_order_relaxed) && pc_settings().current().language == 6;
}

std::unique_ptr<Provider> make_audio_provider() { return std::make_unique<AudioProvider>(); }
std::unique_ptr<Provider> make_controls_provider() { return std::make_unique<ControlsProvider>(); }
std::unique_ptr<Provider> make_gameplay_provider() { return std::make_unique<SaveDataProvider>(); }
std::unique_ptr<Provider> make_system_provider(std::string build) {
    return std::make_unique<SystemProvider>(std::move(build));
}

void add_default_providers(std::string build_info) {
    Registry& r = registry();
    if (!r.has(Category::Display)) r.add(make_display_provider());
    if (!r.has(Category::Graphics)) r.add(make_graphics_provider());
    if (!r.has(Category::Audio)) r.add(make_audio_provider());
    if (!r.has(Category::Controls)) r.add(make_controls_provider());
    if (!r.has(Category::Gameplay)) r.add(make_gameplay_provider());
    if (!r.has(Category::System)) r.add(make_system_provider(std::move(build_info)));
}

}  // namespace gekko::settings
