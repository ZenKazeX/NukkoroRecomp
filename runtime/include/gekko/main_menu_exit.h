#pragma once

// EXIT in the title's main menu (user request 2026-11-27): a fourth entry under 1P GAME / 2P GAME / OPTIONS that
// asks "Exit the game?" in the game's own Yes / No caution window and, on Yes, closes the game the way closing its
// window does. A DELIBERATE ADDITION to the original game. Evidence and addresses: docs/re/main-menu-exit/README.md.
//
// Original behaviour: the main menu is scene "Menu" of csdFiles/honban.gncp; its entries are pre-drawn pictures on the
// language's honban_<lang>.txd sheet "gamemenu"; 0x802BDF0C runs it with a cursor 0..2 (the literal clamp at
// 0x802BE0D8) and plays the motion the table 0x80558A48 names for the cursor (Menu_1P / Menu_2P / Menu_Options:
// the chosen entry white, the others dark red).
//
// Native addition, three parts, all built from the player's own disc (the repo holds no game data):
//  * the picture (add_exit_picture): the language's word cut letter by letter from that language's own gamemenu
//    sheet (with outline columns cut from the same sheet between the letters), the palette values copied as they are,
//    written centred into an empty corner of the same file's TitleParts texture (texture 0 of the scene). The txd keeps
//    its size; gamemenu, which the button-glyph patch rewrites, is not touched.
//  * the question: "Exit the game?" in the text language (JP, EN, FR, SP, GE, IT); Yes / No are the game's own
//    messages 6047 / 6048 of that language.
//  * the entry (add_exit_entry): scene Menu gets a cast "exit" (a copy of "options", one step lower, on the new cell)
//    whose tracks in every motion are those of "options", and a motion "Menu_Exit" (Menu_Options with EXIT chosen).
//  * the code (install): a native owner of 0x802BDF0C that declines - the original words run - unless the frame
//    involves EXIT: the cursor on EXIT, Down pressed alone on OPTIONS, or its own question / leaving sub-states. Then it
//    does the frame's work through the original functions with four entries. EXIT asks the caution window 0x2A (the
//    memory card's Yes / No question that starts on No) with the question written over that window's message in the
//    loaded text table while it is shown, and put back once it has closed. Yes calls `on_exit` (the host then stops
//    the game as for a closed window); No, or B, returns to the menu on EXIT.
//
// A run that never brings EXIT into a frame executes the original words of the menu; the guest memory that differs is
// the two front-end files' content (the larger honban.gncp, the TitleParts texels) and what the CSD scene builds from
// them (one more cast). SHADOW_MAIN_MENU_EXIT=0 leaves all three parts out.

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "gekko/types.h"

namespace gekko {

class Cpu;

namespace main_menu_exit {

// SHADOW_MAIN_MENU_EXIT=0 turns the addition off (files, owner).
bool enabled();

// The language suffixes of honban_<lang>.txd, and the files.
constexpr const char* kLanguages[] = {"EN", "FR", "GE", "IT", "JP", "SP"};
constexpr const char* kProjectPath = "csdFiles/honban.gncp";
constexpr const char* kLetterSheetPath = "csdFiles/honban_EN.txd";  // the English sheet (EN, and JP's word)
std::string picture_path(const char* language);                     // csdFiles/honban_<lang>.txd

// Whether `letters_txd` (honban_EN.txd) holds the PAL English gamemenu sheet the letters are cut from.
bool letters_available(const std::vector<u8>& letters_txd, std::string& error);

// `txd` (honban_<language>.txd) with the language's EXIT word written into its TitleParts texture (same size): the
// word cut from the language's own gamemenu sheet (EN EXIT, FR SORTIR, GE ENDE, IT TERMINA, SP SALIR), or, for JP and
// for a sheet that is not the PAL one, the English word cut from `english_txd`. `word_language` (optional) tells
// whose word was written. Refused when the English sheet is needed and is not the PAL one, or the area is not empty.
bool add_exit_picture(const std::vector<u8>& txd, const char* language, const std::vector<u8>& english_txd,
                      std::vector<u8>& out, std::string& error, std::string* word_language = nullptr);

// `gncp` (honban.gncp, retail layout) with the EXIT cast and the Menu_Exit motion in scene Menu. Refused when the
// scene is not the retail one (or already has EXIT).
bool add_exit_entry(const std::vector<u8>& gncp, std::vector<u8>& out, std::string& error);

// What the owner did (reports, tests).
struct Stats {
    u64 declined = 0;          // frames of 0x802BDF0C run by the original words
    u64 owned = 0;             // frames done natively (EXIT involved)
    u64 questions = 0;         // times the question was opened
    u64 answered_no = 0;       // ... answered No or cancelled with B
    u64 answered_yes = 0;      // ... answered Yes
    u64 text_failures = 0;     // the question's text could not be placed (the window was not opened)
};

struct State {
    std::atomic<bool> entry_ready{false};  // the project was built with the entry (set by the mount's builder)
    std::function<void()> on_exit;         // Yes: stop the game as a closed window does
    Stats stats;
    // The message text the question overwrote while it is shown: its guest address and the original units.
    u32 saved_text = 0;
    std::vector<u16> saved_units;
};

// Installs the owner of 0x802BDF0C (nothing when !enabled()). `state` must outlive the Cpu.
void install(Cpu& cpu, State& state);

}  // namespace main_menu_exit
}  // namespace gekko
