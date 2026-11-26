#pragma once

// The look of the Settings program and the Mod Manager (see appearance.h for the file that chooses light or dark and
// the text size). It follows Windows 11's own design (learn.microsoft.com/windows/apps/design): Segoe UI Variable,
// layered surfaces (the window, then cards on it), rounded controls, the player's accent colour on the main button, on
// ticked boxes, switches and sliders, and a title bar in the window's own colour.
//
// Windows' controls are used where they look right (lists, combo boxes, edits, scroll bars, menus, in their dark styles
// in dark); push buttons, check boxes, switches and sliders are drawn here, the same in light and dark. With Windows'
// high contrast mode on, nothing is drawn here and the programs use the system colours.
//
// Every text that reaches the player is passed in by the program; this file holds none.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>

#include <string>
#include <vector>

#include "appearance.h"

namespace ui {

struct Palette {
    bool dark = false;
    bool custom = false;        // our own drawing is on (off in high contrast)
    COLORREF window = 0;        // the window's background
    COLORREF surface = 0;       // a card on the window
    COLORREF surface_stroke = 0;
    COLORREF text = 0;          // labels
    COLORREF muted = 0;         // explanations, things switched off
    COLORREF field = 0;         // edits, lists, drop-down lists
    COLORREF field_text = 0;
    COLORREF border = 0;        // an empty check box, a switch that is off, a slider's track
    COLORREF control = 0;       // a push button
    COLORREF control_hot = 0;   // under the pointer
    COLORREF control_pressed = 0;
    COLORREF control_stroke = 0;
    COLORREF selected = 0;      // the chosen item of a navigation list
    COLORREF accent = 0;        // Windows' accent colour, in the shade Windows uses for this light or dark
    COLORREF accent_text = 0;   // text on the accent
};

// Whether Windows' app mode is dark (Settings > Personalisation > Colours), and high contrast is off.
bool windows_prefers_dark();
// Whether `theme` means dark colours now.
bool is_dark(Theme theme);

// Chooses the colours used from now on, for the whole program: its menus and drop-down lists too.
// shadow: the Shadow theme, dark with the game's red instead of Windows' accent colour.
void set_dark(bool dark, bool shadow = false);
const Palette& colors();
// Whether set_dark(dark, shadow) would now give other colours than the ones in use: another light or dark, Windows'
// accent colour or high contrast mode changed. Windows announces colour changes often; this tells the real ones.
bool colors_differ(bool dark, bool shadow);
HBRUSH window_brush();
HBRUSH surface_brush();
HBRUSH field_brush();

// Font faces: Windows 11's Segoe UI Variable where it is installed, Segoe UI before; the icon font (Segoe Fluent
// Icons, or Segoe MDL2 Assets on Windows 10), empty when there is none.
const wchar_t* text_face();
const wchar_t* title_face();
const wchar_t* icon_face();

// A top-level window in the current colours: its title bar (the window's own colour, rounded corners) and its scroll
// bars, then each child control.
void apply_to_window(HWND window);
// One control in the current colours, by its kind. A list with check boxes loses its ticks (Windows rebuilds the
// boxes): fill it again afterwards.
void apply_to_control(HWND control);

// How a push button or check box is drawn: the main button of a window (the accent colour), a check box drawn as an
// on/off switch, an icon (a character of icon_face()) before a button's text.
void set_primary(HWND button);
void set_switch(HWND check_box);
void set_icon(HWND button, wchar_t glyph);

// The answer to WM_CTLCOLORSTATIC, WM_CTLCOLORBTN, WM_CTLCOLOREDIT, WM_CTLCOLORLISTBOX and WM_CTLCOLORDLG: sets the
// colours on `dc` and returns the background brush. `muted`: the explanation grey; `on_surface`: the control is on a
// card.
LRESULT control_color(UINT message, HDC dc, HWND control, bool muted = false, bool on_surface = false);

// NM_CUSTOMDRAW from a push button, check box or slider: drawn here, *result is the answer and the return is true.
// False (high contrast, or another control): Windows draws it.
bool custom_draw(const NMHDR* header, LRESULT* result);

// A card: a rounded surface with a thin edge, for a group of controls.
void paint_card(HDC dc, const RECT& r, int radius);
// A navigation item (an owner-drawn list's row): `chosen` gets the highlight and the accent mark; `glyph` (0: none)
// is drawn in icon_face() before the text, or centred alone when `text` is empty (a narrow list).
void paint_nav_item(HDC dc, const RECT& r, const wchar_t* text, wchar_t glyph, bool chosen, bool focused, HFONT font,
                    HFONT icons);

// A font's height (CreateFont's negative em height) for `points` at `dpi`, at `text_size` percent.
int font_height(int points, int dpi, int text_size);

// ---- fitting text (zoom makes the text larger, never the window: the programs lay out to the room they have) --------

// A button with an icon (set_icon) drawn as the icon alone, when its words do not fit. Its text stays its name for
// screen readers; give it a tooltip. can_show_icon_only: false in high contrast, where Windows draws the words.
void set_icon_only(HWND button, bool icon_only);
bool can_show_icon_only(HWND button);
// The size `text` needs in `font`, in pixels: on one line, or wrapped to `wrap` pixels (a word longer than that makes
// it wider).
SIZE text_extent(HFONT font, const std::wstring& text, int wrap = 0);
// The width a push button needs for its icon and words (or its icon alone), and a check box or switch for its mark and
// words on one line; the height a BS_MULTILINE check box needs at `width`. In pixels, in the control's font.
int button_width(HWND button);
// The same as if it were (or were not) drawn as its icon alone, to measure before choosing.
int button_width(HWND button, bool icon_only);
int check_box_width(HWND check_box);
int check_box_height(HWND check_box, int width);
// Drop-down lists (combo boxes) in Windows 11's style, at the size of their font: create them with CBS_OWNERDRAWFIXED |
// CBS_HASSTRINGS, answer WM_MEASUREITEM and WM_DRAWITEM with measure_item() and draw_item() (true when they were a
// combo box's), and call fit_dropdown() after a new font (apply_to_control() does it). The closed box is a rounded face
// with a chevron, the open list has roomy rows with a rounded highlight. dropdown_chrome(): the width a closed box
// needs besides its words, in pixels.
void fit_dropdown(HWND combo);
bool measure_item(MEASUREITEMSTRUCT* measure);
bool draw_item(const DRAWITEMSTRUCT* draw);
int dropdown_chrome(HFONT font);
// The visible child controls of `parent` whose text does not fit them (statics, buttons, check boxes, the chosen item
// of combo boxes) or that cross the window's side, and its bottom too when `whole_height` (a window that does not
// scroll). One line each, for the programs' --check-layout.
std::vector<std::wstring> find_clipped(HWND parent, bool whole_height);

// GDI+ must be running for the drawing above (the program starts it once).
void start_drawing();
void stop_drawing();

}  // namespace ui
