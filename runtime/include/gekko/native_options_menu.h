#pragma once

// The PC Options menu: six categories on the game's own Options screen (docs/OPTIONS_MENU_OVERHAUL.md, the user's
// directive of 2026-11-05; the design is docs/OPTIONS_MENU_ARCHITECTURE.md). This is the presentation: the settings
// themselves (definitions, values, apply, persistence, the 15 s display trial) are gekko::settings::registry()'s
// (gekko/settings_registry.h, gekko/settings_display.h, gekko/settings_game.h).
//
// The game keeps the screen: background, header, the 3 x 2 tile grid with its cursor, poses, open / close motions,
// sounds, the page plate, the prompts, B's restore-and-close, the fades. Natively owned (the entries the Display tile
// used, now for every item):
//   * setup 0x802B833C: the hidden-tile flags cleared (all six tiles shown and reachable);
//   * the help line: the screen sets it every frame through 0x802BF228 (from 0x802B8F64 / 0x802B9070) with the item's
//     help message; it gets the category's description on the grid and the highlighted setting's in the page - new
//     messages 8000.. inserted into Advertise_<lang>.fnt at mount, so the game draws them in its own font and place;
//   * the page rows (0x802B8BC4): published to the overlay (options_display_overlay.h), never the game's;
//   * the page input: up / down / left / right (the repeat query from 0x802B96D4; 0x802B9754 answers 0), A (the edge
//     query from 0x802B97F4), and B (from 0x802B9980) while the sound test list or the display confirmation is open.
//
// Categories (item = row * 2 + column): 0 Display, 1 Graphics, 2 Audio, 3 Controls, 4 Save Data, 5 System. Each page
// lists its category's settings and ends with "Restore defaults" (System: "Restore all defaults"). Left / right
// change a pending value, A applies the category (a window mode or resolution change asks for confirmation within
// 15 s), B leaves the page and drops what is pending (the game's own B).
//
// The game's own options and commands (speaker output, vibration, text language, autosave, sound test, Load Game Data) run
// through GameOptions (gekko/settings_game.h), implemented here on the game thread: values written into the screen's
// value slots and applied by the game's 0x802B85D8 (a language change through the game's fade and state 3, as its
// Subtitle page does), the sound test as 0x802B9850's calls in a nested list, Load as the game's state 4.

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "gekko/native_options_display.h"
#include "gekko/settings_game.h"
#include "gekko/settings_registry.h"
#include "gekko/types.h"

namespace gekko {
class Cpu;
}

namespace gekko::options_menu {

// The guest's entries and data (main.dol, PAL GUPP8P).
constexpr u32 kSetup = 0x802B833Cu;
constexpr u32 kPage = 0x802B964Cu;
constexpr u32 kDrawRows = 0x802B8BC4u;
constexpr u32 kRepeatPress = 0x802B810Cu;
constexpr u32 kEdgePress = 0x802B8168u;
constexpr u32 kSetText = 0x802BF228u;        // (holder, message, 1, node, 0): a text node by message id
constexpr u32 kHelpLineCall = 0x802B8F64u;   // return addresses of the update's help-line calls
constexpr u32 kHelpLineCall2 = 0x802B9070u;
constexpr u32 kPageUpQuery = 0x802B96D4u;
constexpr u32 kPageDownQuery = 0x802B9754u;
constexpr u32 kPageAQuery = 0x802B97F4u;
constexpr u32 kPageBQuery = 0x802B9980u;
constexpr u32 kCursor = 0x80585528u;         // {row, column}, value slots + 0x20 + 4 * item
constexpr u32 kGrid = 0x804FF7A8u;
constexpr u32 kSettings = 0x8057E760u;       // the live settings: + 0x58 text language, + 0x78 speaker, + 0x7C vibration
constexpr u32 kCategoryCount = 6u;
constexpr u32 kVisibleRows = 7u;             // the page's text slots (fontnull text1..text7)

// The help line's messages (inserted into every front-end table at mount): the category descriptions, the
// confirmation and sound test lines, and per category up to 31 settings + its "Restore defaults" row (index 31).
constexpr u32 kCategoryHelpBase = 8000u;
constexpr u32 kConfirmHelp = 8090u;
constexpr u32 kSoundTestHelp = 8091u;
constexpr u32 kHelpBase = 8100u;
constexpr u32 kRowsPerCategory = 32u;
constexpr u32 help_id(u32 category, u32 index) { return kHelpBase + category * kRowsPerCategory + index; }

// The messages to insert into a language's front-end table, from the registry's settings (English until translated).
// `hungarian`: the Hungarian texts (the Hungarian variant of the English table; Magyar runs on the English slot).
std::vector<std::pair<u32, std::u16string>> help_messages(options_display::Language language, bool hungarian = false);

// A language's front-end table as mounted (the menu reads the game's value texts from it: speaker output names).
void set_front_end_table(options_display::Language language, std::vector<u8> table);
// The Hungarian variant of the English front-end table (read while Magyar is selected).
void set_hungarian_front_end_table(std::vector<u8> table);

class Menu : public settings::GameOptions {
public:
    Menu();
    ~Menu() override;
    // The language the rows are drawn in (the game's text language at each Options entry); a change rebuilds the font.
    void set_language(options_display::Language language) noexcept {
        if (language == language_) return;
        language_ = language;
        font_ready_ = font_failed_ = false;
    }
    void set_font_files(options_display::Language language, std::vector<u8> txd,
                        std::vector<std::vector<u8>> glyph_lists, std::u16string on, std::u16string off);
    // The game's track names (messages 6100 + track) of a language's table.
    void set_track_names(options_display::Language language, std::vector<std::u16string> names);
    // The Hungarian front end (the Hungarian variant of Advertise_EN, gekko/native_hungarian_text.h): while Magyar is
    // selected (settings::hungarian_mounted()) and the game reports English, the rows are drawn with these sheets and
    // the Hungarian texts. Set from that variant's build; until then the rows stay English.
    void set_hungarian_font_files(std::vector<u8> txd, std::vector<std::vector<u8>> glyph_lists, std::u16string on,
                                  std::u16string off, std::vector<std::u16string> track_names);
    // The description messages are in the front-end tables (help_messages): the help line may use them.
    void set_messages_ready(bool ready) noexcept { messages_ready_ = ready; }

    using Sound = void (*)(void* context, Cpu& cpu, u32 id);
    void set_sound(Sound sound, void* context) noexcept {
        sound_ = sound;
        sound_context_ = context;
    }

    bool setup(Cpu& cpu);         // kSetup: every tile shown; declines
    bool page(Cpu& cpu);          // kPage: notes that the page has the cursor; declines
    bool draw_rows(Cpu& cpu);     // kDrawRows: publishes the category's rows; the game's are not drawn
    bool repeat_press(Cpu& cpu);  // kRepeatPress from the page's up / down queries
    bool edge_press(Cpu& cpu);    // kEdgePress from the page's A / B queries
    bool set_text(Cpu& cpu);      // kSetText from the help-line calls: the description's message; declines

    // settings::GameOptions (valid while a hook runs: the CPU is attached for its duration).
    bool available() const override { return cpu_ != nullptr; }
    unsigned speaker() const override;
    bool vibration() const override;
    unsigned language() const override;
    bool autosave() const override;
    bool set_autosave(bool on, std::string* error) override;
    unsigned voice() const override;
    bool apply(unsigned speaker, bool vibration, unsigned language, unsigned voice, std::string* error) override;
    void reload_text_on_next_apply() override { reload_text_ = true; }
    // The dialogue-voice archive is the switchable one: a voice change needs no reload (set at the mount).
    void set_seamless_voice(bool on) noexcept { seamless_voice_ = on; }
    bool run(std::string_view action, std::string* error) override;

    struct Report {
        u64 setups = 0, row_draws = 0, moves = 0, changes = 0, applies = 0, apply_failures = 0, saves = 0,
            save_failures = 0, font_failures = 0, confirms = 0, reverts = 0, game_applies = 0, tracks_played = 0,
            loads = 0;
    };
    const Report& report() const noexcept { return report_; }
    const std::string& last_error() const noexcept { return last_error_; }

private:
    struct FontFiles {
        std::vector<u8> txd;
        std::vector<std::vector<u8>> glyph_lists;
        std::u16string on, off;
    };
    struct Attach;  // attaches the CPU (GameOptions) for a hook's duration
    u32 current_item(Cpu& cpu) const;
    std::vector<settings::SettingView> rows() const;  // the open category's settings + its defaults row
    std::u16string label_text(const settings::SettingView& v) const;
    std::u16string value_text(const settings::SettingView& v, bool pending) const;
    void page_input(Cpu& cpu, u32 impl);
    void press_a(Cpu& cpu, u32 impl);
    void apply_category(Cpu& cpu);
    void restore_defaults(Cpu& cpu, bool every_category);
    void play_track(Cpu& cpu, u32 track);
    void publish(Cpu& cpu, bool focused);
    void hide(Cpu& cpu);
    bool ensure_font();
    void sound(Cpu& cpu, u32 id);
    bool call(Cpu& cpu, u32 function, u32 r3, u32 r4);

    options_display::Language language_ = options_display::Language::EN;
    std::array<FontFiles, 6> files_{};
    std::array<std::vector<std::u16string>, 6> tracks_{};
    // The Hungarian front end (set_hungarian_font_files); written by the asset build, read on the game thread.
    mutable std::mutex hungarian_mutex_;
    FontFiles hungarian_files_{};
    std::vector<std::u16string> hungarian_tracks_;
    bool hungarian() const;          // Magyar is selected, the game reports English and the Hungarian sheets are here
    u32 column() const;              // the menu texts' column: language_'s, or 6 while hungarian()
    bool font_hungarian_ = false;    // the overlay's font was built from the Hungarian sheets
    bool reload_text_ = false;       // reload_text_on_next_apply
    bool seamless_voice_ = false;    // set_seamless_voice
    u32 logged_help_ = ~0u;          // the help line's last logged message
    bool font_ready_ = false, font_failed_ = false, messages_ready_ = false;
    std::u16string on_off_[2];
    Sound sound_ = nullptr;
    void* sound_context_ = nullptr;

    Cpu* cpu_ = nullptr;  // attached while a hook runs (GameOptions)
    u32 impl_ = 0;        // the screen's impl (the page handler's r31; setup's r3)
    bool page_ran_ = false;
    bool open_ = false;   // the page has the cursor and the category's pending values were begun
    u32 item_ = 0;        // the category whose rows are shown
    u32 cursor_ = 0, scroll_ = 0;
    // A display change the window owner refused (settings::Registry::last_failure): shown on the page for a while.
    std::uint64_t failure_seen_ = 0;
    std::uint64_t failure_until_ticks_ = 0;
    std::string failure_text_;
    void log_engine_state(Cpu& cpu);  // stderr: the settings object's option values, when they change
    u32 logged_engine_[6] = {};
    int play_rank_ = -1;  // a sound test play waiting for the game's own A path (edge_press)
    bool logged_engine_valid_ = false;
    bool page_closes_ = false;  // a language change or Load sent the screen to its state 3 / 4
    // The sound test list (nested screen): the unlocked tracks, its cursor and scroll.
    bool sound_test_ = false;
    std::vector<u32> unlocked_;
    u32 list_cursor_ = 0, list_scroll_ = 0;

    Report report_{};
    std::string last_error_;
};

}  // namespace gekko::options_menu
