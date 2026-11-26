#pragma once

// Light and dark colours for the Settings program and the Mod Manager (see appearance.h for the file that chooses).
// Light is Windows' own look, from the system colours. Dark uses Windows' dark control styles where it has them (push
// buttons, combo boxes, edits, lists, scroll bars, menus, the title bar) and draws the rest here: check boxes, sliders
// and the tab strip. With Windows' high contrast mode on, the programs always use the system colours.
//
// Every text that reaches the player is passed in by the program; this file holds none.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>

#include "appearance.h"

namespace ui {

struct Palette {
    bool dark = false;
    COLORREF window = 0;      // the window's background
    COLORREF text = 0;        // labels
    COLORREF muted = 0;       // explanations, things switched off
    COLORREF field = 0;       // edits, lists, drop-down lists
    COLORREF field_text = 0;
    COLORREF border = 0;      // lines, the empty check box, the slider's track
    COLORREF accent = 0;      // a ticked check box, the slider's knob, the chosen tab
    COLORREF accent_text = 0; // the tick on the accent
};

// Whether Windows' app mode is dark (Settings > Personalisation > Colours), and high contrast is off.
bool windows_prefers_dark();
// Whether `theme` means dark colours now.
bool is_dark(Theme theme);

// Chooses the colours used from now on, for the whole program: its menus and drop-down lists too.
void set_dark(bool dark);
const Palette& colors();
HBRUSH window_brush();
HBRUSH field_brush();

// A top-level window in the current colours: its title bar and its own scroll bars, then each child control.
void apply_to_window(HWND window);
// One control in the current colours, by its kind: push buttons, check boxes, combo boxes, edits, lists, tab strips.
// A list with check boxes loses its ticks (Windows rebuilds the boxes): fill it again afterwards.
void apply_to_control(HWND control);

// The answer to WM_CTLCOLORSTATIC, WM_CTLCOLORBTN, WM_CTLCOLOREDIT, WM_CTLCOLORLISTBOX and WM_CTLCOLORDLG: sets the
// colours on `dc` and returns the background brush. `muted`: the explanation grey instead of the text colour.
LRESULT control_color(UINT message, HDC dc, HWND control, bool muted = false);

// NM_CUSTOMDRAW from a check box or a slider: in dark colours it is drawn here, *result is the answer and the return
// is true. False (light colours, or another control): Windows draws it.
bool custom_draw(const NMHDR* header, LRESULT* result);

// A font's height (CreateFont's negative em height) for `points` at `dpi`, at `text_size` percent.
int font_height(int points, int dpi, int text_size);

}  // namespace ui
