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

#include "gekko/display_geometry.h"
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
    kMsaa,          // the scene's MSAA samples (Off, 2x, 4x, 8x ... as the GPU supports)
    kAnisotropy,    // anisotropic filtering of the game's filtered textures (Off = the console's linear filter)
    kRowCount,
};
constexpr u32 kVisibleRows = 7u;  // the page's text slots above the select button
// The rows the page shows. Frame rate and frame blending are not offered: the release present loop shows every game
// frame as it comes and applies neither (display_settings.h), and a row must change what it names. Draw distance is
// withheld from the first release (user 2026-10-30: early alpha); display.cfg and --draw-distance still set it.
// The rows the page shows (docs/DISPLAY_SETTINGS_AUDIT.md section 6): four, each with one job. The others are
// display.cfg / command-line keys for tests (MSAA, filtering, the FPS counter, exclusive mode) or withheld (frame rate
// and frame blending: the release present loop applies neither; draw distance: user 2026-10-30, early alpha).
constexpr Row kShownRows[] = {kDisplayMode, kScreenRatio, kRenderResolution, kVsync};
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
    const char* exclusive;  // exclusive fullscreen (the "fullscreen" text names borderless)
};
const Texts& texts(Language language) noexcept;

// The size of the monitor the game window is on (the UI loop publishes it): borderless fullscreen's one resolution
// (it shows at the desktop's size). 0 x 0: not known (no window); borderless then lists the presets.
void set_screen_size(u32 width, u32 height) noexcept;
// The work area of that monitor (the desktop less the taskbar, less the window frame: the largest client a window
// can have there). 0 x 0: not known; windowed then lists every preset.
void set_work_area(u32 width, u32 height) noexcept;
// The MSAA sample counts the GPU supports for the scene (bit n: 2^n samples; render/renderer.h). Until set: 1, 2, 4, 8.
void set_msaa_supported(u32 mask) noexcept;

// The page's model (gekko/display_geometry.h): every setting held as its VALUE (a list only says where a left /
// right step goes; a value no list holds is shown as itself and kept). No row changes another row's value:
//   mode          Windowed / Fullscreen
//   ratio         Auto or a fixed ratio: the picture's shape
//   scale         25..200 % per side of the picture (the resolution scale)
//   V-Sync        On / Off
struct Model {
    struct Size {
        u32 width, height;
    };
    static Model from(const DisplaySettings& settings);
    u32 count(u32 row) const;
    u32 index(u32 row) const;       // the value's place in its row's list
    void step(u32 row, int delta);  // moves the row's value along its list (clamped at the ends); nothing else changes
    std::u16string value_text(u32 row, const Texts& t, const std::u16string& on, const std::u16string& off) const;
    std::vector<double> aspects() const;      // 0 (Auto), the named ratios, a custom one in effect
    std::vector<double> scales() const;       // percent per side; the scale in effect included
    std::vector<u32> msaa_values() const;        // supported sample counts, the one in effect included
    std::vector<u32> anisotropy_values() const;  // 1 (Off), 2, 4, 8, 16
    PixelSize output() const;                 // what the window owner shows the picture in: the window or the screen
    DisplayGeometry geometry() const;         // the picture and the render size for the chosen values
    Size viewport() const;                    // the picture
    DisplaySettings settings(const DisplaySettings& base) const;

    u32 value[kRowCount] = {};  // the index-valued rows: mode, toggles, MSAA, filtering, frame rate, draw distance
    Size window{1280, 720};     // the window's client size in effect (windowed)
    Size screen{0, 0};          // the monitor's size when the page was opened (0 x 0: not known)
    Size work{0, 0};            // the largest client its desktop holds (0 x 0: not known)
    double aspect = 0.0;        // the screen ratio (0 = Auto)
    double scale = 100.0;       // the resolution scale, percent
    u32 msaa_setting = 4;       // the MSAA count in effect (listed even when the GPU lacks it: shown as such)
    std::vector<double> rates;  // the frame rate list (0 = unlocked), the rate in effect included
};
// The work area set_work_area published (0 x 0: not known).
Model::Size work_area() noexcept;

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
        std::u16string label, value;  // a list entry (sound test) has no value; an action row neither
        bool info = false;            // shown, not chosen (build information)
    };
    std::vector<Line> lines;
    std::u16string counter;           // the scroll position, "3/8" (empty when every row fits)
    bool more_above = false, more_below = false;
    std::vector<std::u16string> dialog;  // non-empty: the panel shows these lines instead (the display confirmation)
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

// The game's input state, read as the original queries read it (0x8034A608 / 0x80357068 / 0x80356F98).
bool button_repeats(Cpu& cpu, u32 button);
bool button_pressed(Cpu& cpu, u32 button);

// The six category tiles' art (docs/OPTIONS_MENU_OVERHAUL.md: Display, Graphics, Audio, Controls, Gameplay, System) in
// Options2 of csdFiles/Options_<lang>.txd: cells 41..46 (normal) and 47..52 (selected), 82 x 82 each. False (with the
// reason) when the dictionary is not the one expected.
bool place_tile_art(const std::vector<u8>& txd, std::vector<u8>& out, std::string& error);

// A front-end table (Advertise_<lang>.fnt) with new messages inserted in id order (their entry words copied from
// message `template_id`, the help line's); a glyph missing from `glyph_lists` is dropped from the text and counted in
// `missing`. Refused when an id already exists or the table cannot be read.
bool insert_messages(const std::vector<u8>& table, const std::vector<std::vector<u8>>& glyph_lists,
                     const std::vector<std::pair<u32, std::u16string>>& messages, u32 template_id,
                     std::vector<u8>& out, u32& missing, std::string& error);

// One message of a front-end table by its id (the game's lookup: entry word 1 = id * 1000 + 100).
bool message_by_id(const std::vector<u8>& table, u32 id, std::u16string& text);

// The glyph sheets of a language as the overlay draws them (Advertise_<lang>.txd + its .met lists), the language's own
// first and English for what it lacks.
bool build_font(Language language, const std::vector<std::pair<const std::vector<u8>*, const std::vector<std::vector<u8>>*>>& files,
                Font& out);

// Decodes UTF-8 to UTF-16 (the texts above are UTF-8).
std::u16string utf16(std::string_view utf8);

}  // namespace gekko::options_display
