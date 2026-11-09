#pragma once

// The Options menu's Display tile (user design of 2026-10-28, tools/options_menu_sim; the RE map is in
// docs/OPTIONS_DISPLAY_TILE.md).
//
// Original behaviour (main.dol, PAL GUPP8P): AdvReal::Option::Phase's screen (setup 0x802B833C, update 0x802B8D88)
// has six tiles in a 3 x 2 grid (0x804FF7A8, item = row * 2 + column). The sixth, the voice language (icn_audio,
// item 5), is authored in csdFiles/Options.gncp but the constructor 0x802BA13C sets impl + 0x26, so setup hides its
// cast (0x8036BBC0(cast, 1)) and the grid never moves onto it: the PAL screen shows five tiles.
//
// Native implementation: the sixth slot is the Display tile. The game keeps the screen, the grid, the tile's pose,
// the help line (message 0 of Advertise_<lang>.fnt, reworded here) and the page's open and close (A on the tile,
// B back, with the game's own restore of the saved settings). Natively owned:
//   * setup: impl + 0x26 is cleared at entry, so the tile is shown and the grid reaches it;
//   * the page's rows (0x802B8BC4 for item 5): the game's two voice-language rows are not drawn; the Display rows
//     are, by the overlay (options_display_overlay.h) in the game's own glyph sheets;
//   * the page's input for item 5: the page handler 0x802B964C asks its button queries (0x802B810C repeat,
//     0x802B8168 edge) for up, down and A; for item 5 those calls are served here, and the Display rows read the
//     game's own input state the same way (0x8034A608 / 0x80357068's rules, mirrored), so held buttons repeat on the
//     game's clock. The voice language value (cursor + 0x34) is never written, so the game's save check
//     (0x802B8828) and save (0x802B85D8) see nothing changed by this tile.
//   * the tile's art: two cells of Options2 in csdFiles/Options_<lang>.txd (cells 46 and 52) hold the TV icon.
//
// Settings go through gekko::display_settings() (display_settings.h): left / right change the row's value, A applies
// every changed value and saves display.cfg.

#include <array>
#include <chrono>
#include <utility>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "gekko/display_settings.h"
#include "gekko/types.h"

namespace gekko {
class Cpu;
}

namespace gekko::options_display {

// The guest's entries and data.
constexpr u32 kSetup = 0x802B833Cu;         // (impl): builds the casts, hides the disabled tiles
constexpr u32 kPage = 0x802B964Cu;          // (impl): the open page's input
constexpr u32 kDrawRows = 0x802B8BC4u;      // (impl, item, focused): the page's text rows
constexpr u32 kRepeatPress = 0x802B810Cu;   // (button, 4) -> bool: pressed or repeating
constexpr u32 kEdgePress = 0x802B8168u;     // (button, 4) -> bool: pressed this frame
constexpr u32 kPageUpQuery = 0x802B96D4u;   // return addresses of the page handler's queries
constexpr u32 kPageDownQuery = 0x802B9754u;
constexpr u32 kPageAQuery = 0x802B97F4u;
constexpr u32 kCursor = 0x80585528u;        // {row, column}, + 0x20 + 4 * item = the item's value
constexpr u32 kGrid = 0x804FF7A8u;          // u32[3][2] items
constexpr u32 kItem = 5u;                   // the Display tile's slot (icn_audio)
constexpr u32 kHiddenFlag = 0x26u;          // impl + 0x26: item 5 hidden
constexpr u32 kTextOn = 0x1Eu;              // impl + 0x1E: the page draws text
constexpr u32 kRepeatCountdown = 0x34u;     // impl + 0x34: frames before the next move is read

// The Display page.
enum Row : u32 {
    kDisplayMode = 0,
    kScreenRatio,
    kResolution,
    kRenderResolution,
    kFrameRate,
    kVsync,
    kFrameBlending,
    kDrawDistance,
    kFpsCounter,
    kRowCount,
};
constexpr u32 kVisibleRows = 7u;  // the page's text slots above the select button
// The rows the page shows. Frame rate and frame blending are not offered: the release present loop shows every game
// frame as it comes and applies neither (display_settings.h), and a row must change what it names.
constexpr Row kShownRows[] = {kDisplayMode, kScreenRatio, kResolution, kRenderResolution, kVsync, kDrawDistance,
                              kFpsCounter};
constexpr u32 kShownCount = static_cast<u32>(sizeof(kShownRows) / sizeof(kShownRows[0]));

// The six languages of the front-end (Advertise_<lang>), in this order.
enum class Language : u8 { EN, FR, GE, IT, JP, SP };
constexpr std::array<std::string_view, 6> kLanguageSuffix = {"EN", "FR", "GE", "IT", "JP", "SP"};

// Texts of the Display page in one language (UTF-8). ON / OFF are the game's own (front-end messages 346 / 347).
struct Texts {
    const char* help;
    const char* labels[kRowCount];
    const char* windowed;
    const char* fullscreen;
    const char* automatic;
    const char* unlocked;
    const char* original;
    const char* maximum;
};
const Texts& texts(Language language) noexcept;

// The resolution presets the page offers and the family a size belongs to (the launcher passes
// render/display_modes.h's: the core does not link the renderer). Until set, the list holds only the size in effect.
struct Preset {
    u32 width, height;
    const char* family;  // "16:9", "4:3", ...
};
using FamilyOf = const char* (*)(u32 width, u32 height);
void set_resolution_presets(std::vector<Preset> presets, FamilyOf family_of);

// The page's model: the value index of every row, over the lists Model::from built from the settings in effect.
struct Model {
    struct Size {
        u32 width, height;
    };
    static Model from(const DisplaySettings& settings);
    u32 count(u32 row) const;
    void step(u32 row, int delta);  // clamps at the list's ends; keeps the dependent rows valid
    std::u16string value_text(u32 row, const Texts& t, const std::u16string& on, const std::u16string& off) const;
    std::vector<Size> resolutions() const;  // of the chosen ratio, by area (the size in effect kept if not a preset)
    std::vector<u32> render_percents() const;
    DisplaySettings settings(const DisplaySettings& base) const;

    u32 value[kRowCount] = {};
    Size custom{0, 0};              // the window size in effect when it is no preset (0 x 0: none)
    std::vector<double> rates;      // the frame rate list (0 = unlocked), the rate in effect included
};

// What the overlay draws (UI thread reads, game thread writes).
struct Glyph {
    u32 sheet;                  // index into Font::sheets
    u16 x, y, width, height;    // the glyph's cell
    u16 ink_left, ink_right;    // ink columns within the cell (proportional Latin spacing)
};
struct Font {
    Language language = Language::EN;
    struct Sheet {
        u32 width = 0, height = 0;
        std::vector<u8> rgba;   // as stored: grey glyphs on black, opaque
    };
    std::vector<Sheet> sheets;
    std::vector<std::pair<char16_t, Glyph>> glyphs;  // sorted by code
    u32 serial = 0;             // changes when the font changes
    const Glyph* find(char16_t code) const noexcept;
};
struct Page {
    bool visible = false;
    bool focused = false;       // the page has the cursor (rows can be chosen)
    u32 cursor = 0, scroll = 0;
    struct Line {
        std::u16string label, value;
    };
    std::vector<Line> lines;
    u64 game_ticks = 0;         // the game clock when the game drew (or stopped drawing) the page
};
// The page as the game drew it, frame by frame: every row draw of the screen publishes the Display page's state at
// that game time (hidden when the game draws another tile's rows or no text), so a presented frame shows exactly the
// page of the game frame it is (FramePacket::game_ticks), never one before or after it.
struct Shared {
    std::mutex mutex;
    Font font;
    static constexpr u32 kHistory = 16;  // > the frames between a row draw and its present (the packet queue)
    std::array<Page, kHistory> pages{};
    u32 next = 0;
    void push(Page page);
    // The page the game frame that ended at `game_ticks` drew: the last one published by then; nullptr when none
    // was published within the frame before it (the screen is gone) or the last one is hidden.
    const Page* at(u64 game_ticks) const;
};
Shared& shared() noexcept;

// The game-thread owner.
class Tile {
public:
    void set_language(Language language) noexcept { language_ = language; }
    // A language's glyph sheets (Advertise_<lang>.txd and its .met lists) and its ON / OFF texts, taken at the asset
    // mount (System::mount_assets reads them while it rewords the help line).
    void set_font_files(Language language, std::vector<u8> txd, std::vector<std::vector<u8>> glyph_lists,
                        std::u16string on, std::u16string off);

    bool setup(Cpu& cpu);            // kSetup: clears the hidden flag, declines (the game's setup runs)
    bool page(Cpu& cpu);             // kPage: notes that the page handler ran, declines
    bool draw_rows(Cpu& cpu);        // kDrawRows: serves item 5
    bool repeat_press(Cpu& cpu);     // kRepeatPress: serves the page handler's up / down for item 5
    bool edge_press(Cpu& cpu);       // kEdgePress: serves the page handler's A for item 5

    // The menu sound service (the System plays through the native SE manager).
    using Sound = void (*)(void* context, Cpu& cpu, u32 id);
    void set_sound(Sound sound, void* context) noexcept {
        sound_ = sound;
        sound_context_ = context;
    }

    struct Report {
        u64 setups = 0, row_draws = 0, moves = 0, changes = 0, applies = 0, apply_failures = 0, saves = 0,
            save_failures = 0, font_failures = 0;
    };
    const Report& report() const noexcept { return report_; }
    const std::string& last_error() const noexcept { return last_error_; }

private:
    struct FontFiles {
        std::vector<u8> txd;
        std::vector<std::vector<u8>> glyph_lists;
        std::u16string on, off;
    };
    u32 current_item(Cpu& cpu) const;
    void page_input(Cpu& cpu, u32 impl);
    void apply(Cpu& cpu);
    void publish(Cpu& cpu, bool focused);
    void hide(Cpu& cpu);
    bool ensure_font();
    void sound(Cpu& cpu, u32 id);

    Language language_ = Language::EN;
    std::array<FontFiles, 6> files_{};
    bool font_ready_ = false, font_failed_ = false;
    std::u16string on_off_[2];       // the language's ON and OFF
    Sound sound_ = nullptr;
    void* sound_context_ = nullptr;
    bool page_ran_ = false;          // the page handler ran since the last row draw
    bool open_ = false;              // the page has the cursor and the model was taken from the settings
    Model model_{};
    Model applied_{};                // the model of the settings in effect (a row differing from it is pending)
    u32 cursor_ = 0, scroll_ = 0;
    Report report_{};
    std::string last_error_;
};

// The game's input state, read as the original queries read it (0x8034A608 / 0x80357068 / 0x80356F98).
bool button_repeats(Cpu& cpu, u32 button);
bool button_pressed(Cpu& cpu, u32 button);

// The tile's art in Options2 of csdFiles/Options_<lang>.txd: the texture dictionary with cells 46 (normal) and 52
// (selected) holding the Display tile. False (with the reason) when the dictionary is not the one expected.
bool place_tile_art(const std::vector<u8>& txd, std::vector<u8>& out, std::string& error);

// The front-end table (Advertise_<lang>.fnt) with message 0 (the voice-language help line) reworded to the
// Display tile's help line; refused when a glyph of the new text is missing from `glyph_lists`.
bool reword_help(const std::vector<u8>& table, const std::vector<std::vector<u8>>& glyph_lists, Language language,
                 std::vector<u8>& out, std::string& error);

// Decodes UTF-8 to UTF-16 (the texts above are UTF-8).
std::u16string utf16(std::string_view utf8);

}  // namespace gekko::options_display
