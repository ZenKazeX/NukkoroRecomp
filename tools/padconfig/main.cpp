// Settings.exe: the game's settings program ("Settings - Shadow the Hedgehog"). It edits the files the
// game reads when it starts, beside this program: display.cfg (screen and frame rate), options.cfg (sound, language,
// rumble, camera, autosave), pad.cfg (which controller button does what) and controls.cfg (keyboard and mouse).
// docs/PC_SETTINGS_FILES.md lists every key. Only the keys the player changes are written; every other line stays.
// `--folder <dir>` edits the files of another folder (for tests).

#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <xinput.h>

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

#include <array>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "appearance.h"
#include "controller_view.h"
#include "gekko/display_geometry.h"
#include "pad_config.h"
#include "settings_model.h"
#include "theme.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

using namespace padconfig;
namespace sp = shadow::peripheral;

// Every text the player sees is kept in the program as UTF-8 and widened when it is used: a UTF-16 English sentence
// stored in the executable can coincide with the game's own UTF-16 text (its font files), which the release's content
// scan (dev/scan_game_code.py) rightly refuses. Write UI texts as "..."_w, never L"...".
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
std::wstring operator""_w(const char* text, std::size_t length) { return wide(std::string(text, length)); }

const std::wstring kTitle = "Settings - Shadow the Hedgehog"_w;

// Layout (96 dpi units at the normal text size; px() scales them by the screen and the chosen text size), in the
// manner of Windows 11's own Settings: a navigation list on the left (with the zoom and theme choices under it), the
// chosen page's title, and each setting on a card. The pages are laid out in their own units, from x = 0; make() moves
// them right of the navigation list. The bar at the bottom is below the tallest page.
//
// Zoom never changes the window's size: everything is laid out again for the room the window has, in units, so a
// larger zoom is a narrower page. Words wrap, a setting's control goes under its name, the columns of the controller
// and keyboard pages go one under the other, and the navigation list shows only its icons. The page scrolls up and
// down, never sideways.
constexpr int kNavW = 232;                    // the navigation column, with its words
constexpr int kNarrowNavW = 64;               // the navigation column with only its icons
constexpr int kClientW = kNavW - 8 + 1094;    // the window's width at the start, at zoom 100 %
constexpr int kNavItemH = 40, kLookW = kNavW - 28;
constexpr int kSmallestPage = 240;            // a page is laid out at least this wide
constexpr int kPictureW = 668, kPictureH = 578;  // the controller pictures, at their largest

constexpr int kFirstRow = 100, kFirstLive = 200;
enum : int { kThreshold = 300, kThresholdValue, kSwapSticks, kStyle };
enum : int { kTabs = 400, kRestore, kSave, kClose, kStatus, kInfo, kTextSize, kColours, kPageTitle };
constexpr int kFirstField = 1000, kFirstBind = 2000;
constexpr UINT_PTR kPollTimer = 1;
constexpr UINT kMsgLook = WM_APP + 1;  // wparam: ui::Theme, lparam: text size, control to focus
constexpr DWORD kCaptureMs = 6000;

enum Page : int { kDisplay, kFrameRate, kSound, kLanguage, kPadButtons, kPadOptions, kKeyboard, kSaving, kPageCount };
const std::wstring kPageNames[kPageCount] = {"Display"_w,          "Frame rate"_w,         "Sound"_w,
                                               "Language"_w,         "Controller buttons"_w, "Controller options"_w,
                                               "Keyboard and mouse"_w, "Saving"_w};

const struct {
    std::wstring label;
    sp::ButtonStyle style;
} kStyles[] = {{"Automatic"_w, sp::ButtonStyle::Auto},
               {"Original (GameCube)"_w, sp::ButtonStyle::Original},
               {"Xbox"_w, sp::ButtonStyle::Xbox},
               {"PlayStation"_w, sp::ButtonStyle::PlayStation},
               {"Nintendo Switch"_w, sp::ButtonStyle::Switch}};

// What each keyboard action is called on the page, in sp::Action order.
const std::wstring kActionLabels[sp::kActionCount] = {
    "Move forward"_w,      "Move back"_w,          "Move left"_w,        "Move right"_w,      "Walk (hold)"_w,
    "Camera left"_w,       "Camera right"_w,       "Camera up"_w,        "Camera down"_w,     "Jump (A)"_w,
    "Attack (B)"_w,        "Action (X)"_w,         "Chaos power (Y)"_w,  "L button"_w,        "Move sideways (R, hold)"_w,
    "Z button"_w,          "Pause (Start)"_w,      "Back / pause"_w,     "Menu up (D-pad)"_w, "Menu down (D-pad)"_w,
    "Menu left (D-pad)"_w, "Menu right (D-pad)"_w};

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

// One setting on a page: how it is shown and how it reads and changes the settings being edited.
enum class Kind { Check, Combo, Slider, Number, Size };
struct Choice {
    std::wstring label;
    double value;
};
struct Field {
    int page = 0;
    Kind kind = Kind::Check;
    std::wstring label, help;
    std::vector<Choice> choices;          // Combo, Size (presets)
    double low = 0.0, high = 0.0;         // Slider, Number
    std::wstring bad;                     // Number, Size: what a value it refuses is told
    std::function<double()> get;
    std::function<void(double)> set;
    std::function<std::wstring(double)> show;  // Slider: the value beside it; Combo: a value set in the file
    std::function<bool()> enabled;
    int id = 0;
    HWND control = nullptr, value = nullptr;
};

struct App {
    HWND window = nullptr, tabs = nullptr;  // tabs: the navigation list
    HFONT font = nullptr, caption = nullptr, title = nullptr, icons = nullptr;
    int origin_x = 0;     // what make() adds to x: page_x for the pages and the bottom bar, 0 for the navigation
    int bottom = 0;       // where the bar at the bottom starts (below the tallest page)
    int bar_h = 70;       // the bar's height

    // The layout for the window's present width (choose_columns()), in units.
    int layout_w = 0;     // the width laid out for
    bool narrow_nav = false;  // the navigation list shows only its icons; the zoom and theme are in the bottom bar
    int page_x = kNavW - 8, page_w = 1094;  // where a page's x = 0 is, and its width
    int top = 72;         // where a page's content starts, below its title
    RECT picture{};       // the controller pictures, in pixels (before scrolling)
    // The controller buttons page has a scale of its own: the list and the pictures stay side by side and grow or
    // shrink together to fit the window, apart from the text zoom (build_pad_page()). While it is built, px() and
    // units() use its scale (unit_zoom), and its controls its font.
    int unit_zoom = 0;    // 0: the text zoom
    HFONT pad_font = nullptr;
    int pad_font_zoom = 0;
    int layout_h = 0;     // the client height laid out for (the controller page fits it)
    int status_w = 0, status_h = 0, info_h = 0;  // the status lines in the bottom bar

    // Laying out again for a new width moves the controls build() made instead of making them again: build() runs
    // once more with `relayout` set, and make() hands back the controls in the order it made them.
    bool relayout = false;
    std::vector<HWND> made;
    std::size_t next_made = 0;
    bool checking = false;  // --check-layout: zoom changes are not stored
    bool rebuilding = false;  // rebuild_view() is making the controls (no relayout until it is done)
    std::array<std::vector<RECT>, kPageCount> cards;  // each page's cards, in client units
    std::vector<HWND> on_card;  // controls drawn on a card (their background is the card's)
    bool card_open = false;     // make() is adding controls on a card
    int dpi = 96;
    int page = kDisplay;
    std::filesystem::path folder;
    std::array<std::vector<HWND>, kPageCount> page_controls;
    int building = -1;  // the page make() adds controls to; -1: always shown

    // The settings being edited and the ones loaded (or last saved).
    DisplayPrefs display, display_saved;
    OptionPrefs options, options_saved;
    ControlsConfig keys, keys_saved;
    GamepadBinding binding, saved;

    std::vector<Field> fields;
    std::map<int, std::wstring> refused;  // field id -> why the typed text is not used
    bool refreshing = false;

    GetStateFn get_state = nullptr;
    int capturing = -1;  // controller button row
    DWORD capture_end = 0;
    WORD held = 0;
    int key_capture = -1;  // keyboard bind slot: action * kMaxBinds + slot
    DWORD key_capture_end = 0;
    bool connected = false, status_is_message = false, game_running = false;
    DWORD next_game_check = 0;
    std::wstring message;
    ControllerView view;
    bool left_trigger = false, right_trigger = false;

    ui::Appearance look;           // the text size and colours (appearance.cfg)
    int scroll_y = 0;     // how far the window's content is scrolled (only up and down)
    HWND last_focus = nullptr;
} g;

std::wstring number_text(double v) { return wide(format_number(v)); }
// The scale in use: the text zoom, or the controller page's own while it is built.
int zoom_now() { return g.unit_zoom > 0 ? g.unit_zoom : g.look.text_size; }
int px(int v) { return MulDiv(v, g.dpi * zoom_now(), 96 * 100); }
HWND item(int id) { return GetDlgItem(g.window, id); }

// ---- scrolling (when the content is taller than the window) ----------------------------------------------------------

int content_h() { return px(g.bottom + g.bar_h); }
bool on_card(HWND h) { return std::find(g.on_card.begin(), g.on_card.end(), h) != g.on_card.end(); }
// Pixels to units, at the present screen and zoom.
int units(int pixels) { return MulDiv(pixels, 96 * 100, g.dpi * zoom_now()); }

// Moves the content so that y of it is at the window's top. Every control is moved by hand: the window may have
// controls outside its visible part, which ScrollWindowEx leaves where they are.
void scroll_to(int y) {
    RECT client;
    GetClientRect(g.window, &client);
    y = std::clamp(y, 0, std::max(0, content_h() - static_cast<int>(client.bottom)));
    const int dy = g.scroll_y - y;
    if (dy == 0) return;
    g.scroll_y = y;
    SetScrollPos(g.window, SB_VERT, y, TRUE);
    int children = 0;
    for (HWND c = GetWindow(g.window, GW_CHILD); c != nullptr; c = GetWindow(c, GW_HWNDNEXT)) ++children;
    HDWP moves = BeginDeferWindowPos(children);
    for (HWND c = GetWindow(g.window, GW_CHILD); c != nullptr && moves != nullptr; c = GetWindow(c, GW_HWNDNEXT)) {
        RECT r;
        GetWindowRect(c, &r);
        MapWindowPoints(nullptr, g.window, reinterpret_cast<POINT*>(&r), 2);
        moves = DeferWindowPos(moves, c, nullptr, r.left, r.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (moves != nullptr) EndDeferWindowPos(moves);
    InvalidateRect(g.window, nullptr, TRUE);
}

// The scroll bar for the window's present size: none when everything fits. The layout keeps the bar's room either
// way (layout_units()), so showing or hiding it never lays the page out again.
void update_scrollbars() {
    static bool busy = false;  // showing the bar resizes the window, which comes back here
    if (busy) return;
    busy = true;
    RECT client;
    GetClientRect(g.window, &client);
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE;
    info.nMax = content_h() - 1;
    info.nPage = static_cast<UINT>(client.bottom);
    SetScrollInfo(g.window, SB_VERT, &info, TRUE);
    scroll_to(g.scroll_y);
    busy = false;
}

// The window's scroll bar was used: `code` is the WM_VSCROLL request.
void on_scrollbar(int code) {
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_ALL;
    GetScrollInfo(g.window, SB_VERT, &info);
    int pos = info.nPos;
    const int line = px(40), page = static_cast<int>(info.nPage) - line;
    switch (code) {
        case SB_LINEUP: pos -= line; break;
        case SB_LINEDOWN: pos += line; break;
        case SB_PAGEUP: pos -= page; break;
        case SB_PAGEDOWN: pos += page; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = info.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = info.nMax; break;
        default: return;
    }
    scroll_to(pos);
}

// Brings the control with the keyboard focus into view (Tab may reach one outside the visible part).
void show_focus() {
    HWND focus = GetFocus();
    if (focus == g.last_focus) return;
    g.last_focus = focus;
    while (focus != nullptr && GetParent(focus) != g.window) focus = GetParent(focus);
    if (focus == nullptr) return;
    RECT r, client;
    GetWindowRect(focus, &r);
    MapWindowPoints(nullptr, g.window, reinterpret_cast<POINT*>(&r), 2);
    GetClientRect(g.window, &client);
    const int margin = px(12);
    int y = g.scroll_y;
    if (r.top < 0) y += r.top - margin;
    else if (r.bottom > client.bottom) y += std::min(r.top - margin, r.bottom - client.bottom + margin);
    scroll_to(y);
}

// True when the text changed.
bool set_text(HWND h, const std::wstring& text) {
    const int length = GetWindowTextLengthW(h);
    std::wstring now(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(h, now.data(), length + 1);
    now.resize(static_cast<std::size_t>(length));
    if (text == now) return false;
    SetWindowTextW(h, text.c_str());
    return true;
}

std::wstring window_text(HWND h) {
    const int length = GetWindowTextLengthW(h);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(h, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

// ---- the controller ----------------------------------------------------------------------------------------------

void load_xinput() {
    for (const wchar_t* name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
        if (HMODULE module = LoadLibraryW(name)) {
            g.get_state = reinterpret_cast<GetStateFn>(GetProcAddress(module, "XInputGetState"));
            if (g.get_state != nullptr) return;
        }
    }
}

// The first connected controller's buttons, as the game reads the same devices.
bool poll(WORD& buttons) {
    buttons = 0;
    g.left_trigger = g.right_trigger = false;
    if (g.get_state == nullptr) return false;
    for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
        XINPUT_STATE state{};
        if (g.get_state(user, &state) == ERROR_SUCCESS) {
            buttons = state.Gamepad.wButtons;
            g.left_trigger = state.Gamepad.bLeftTrigger > g.binding.trigger_threshold;
            g.right_trigger = state.Gamepad.bRightTrigger > g.binding.trigger_threshold;
            return true;
        }
    }
    return false;
}

// ---- status ------------------------------------------------------------------------------------------------------

bool unsaved() {
    return !(g.display == g.display_saved) || !(g.options == g.options_saved) || !(g.keys == g.keys_saved) ||
           !same(g.binding, g.saved);
}

std::wstring bind_text(const sp::Bind& bind);
void fit_status();

void refresh_status() {
    std::wstring text;
    if (g.capturing >= 0) {
        text = "Press the controller button for "_w + wide(kControls[static_cast<std::size_t>(g.capturing)].name) +
               ". Esc cancels, Delete clears it."_w;
    } else if (g.key_capture >= 0) {
        text = "Press a key or a mouse button for "_w +
               std::wstring(kActionLabels[static_cast<std::size_t>(g.key_capture) / sp::kMaxBinds]) +
               ". Delete clears this box. Wait a few seconds to cancel."_w;
    } else if (g.status_is_message) {
        text = g.message;
    } else if (!g.refused.empty()) {
        text = g.refused.begin()->second;
    } else if (g.page == kPadButtons && !g.connected) {
        text = unsaved() ? "Unsaved changes. No controller found: connect one to choose buttons."_w
                         : "No controller found. Connect one to choose buttons."_w;
    } else if (unsaved()) {
        text = "You have unsaved changes. Click Save to keep them."_w;
    } else if (g.page == kPadButtons) {
        text = "Controller connected. Click a button on the picture or in the list to change it."_w;
    } else {
        text = "No unsaved changes."_w;
    }
    const bool status_changed = set_text(item(kStatus), text);
    const bool info_changed =
        set_text(item(kInfo), g.game_running
                                  ? "The game is running. You can still save: it uses the new settings the next time it starts."_w
                                  : "The game reads these settings when it starts, so changes take effect the next time you start it."_w);
    if (status_changed || info_changed) fit_status();
}

void show_message(const std::wstring& text) {
    g.status_is_message = true;
    g.message = text;
    refresh_status();
}

void changed() {
    g.status_is_message = false;
    refresh_status();
}

bool game_is_running() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const std::wstring game = "ShadowTheHedgehog.exe"_w;
    bool found = false;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !found; ok = Process32NextW(snapshot, &entry))
        found = lstrcmpiW(entry.szExeFile, game.c_str()) == 0;
    CloseHandle(snapshot);
    return found;
}

// ---- the controller buttons page (unchanged behaviour) -----------------------------------------------------------

std::wstring row_text(std::size_t i) { return control_label(g.binding, i); }

void refresh_rows() {
    for (std::size_t i = 0; i < kControls.size(); ++i) {
        const bool waiting = g.capturing == static_cast<int>(i);
        set_text(item(kFirstRow + static_cast<int>(i)), waiting ? "Press a button..."_w : row_text(i));
    }
}

void refresh_pad_options() {
    SendMessageW(item(kThreshold), TBM_SETPOS, TRUE, g.binding.trigger_threshold);
    set_text(item(kThresholdValue), std::to_wstring(g.binding.trigger_threshold));
    CheckDlgButton(g.window, kSwapSticks, g.binding.swap_sticks ? BST_CHECKED : BST_UNCHECKED);
    for (int i = 0; i < static_cast<int>(std::size(kStyles)); ++i)
        if (kStyles[i].style == g.binding.button_style) SendMessageW(item(kStyle), CB_SETCURSEL, i, 0);
}

void stop_capture() {
    g.capturing = -1;
    refresh_rows();
    refresh_status();
}

void start_capture(int index) {
    g.key_capture = -1;
    g.status_is_message = false;
    poll(g.held);
    g.capturing = index;
    g.capture_end = GetTickCount() + kCaptureMs;
    refresh_rows();
    refresh_status();
}

void finish_capture(std::uint16_t mask) {
    assign(g.binding, static_cast<std::size_t>(g.capturing), mask);
    stop_capture();
}

RECT picture_area() {
    return RECT{g.picture.left, g.picture.top - g.scroll_y, g.picture.right, g.picture.bottom - g.scroll_y};
}

// Repaints the pictures only when what they show changed.
void refresh_picture(WORD buttons) {
    ControllerView next;
    next.binding = g.binding;
    next.held = buttons;
    next.left_trigger = g.left_trigger;
    next.right_trigger = g.right_trigger;
    next.selected = g.capturing;
    if (next == g.view) return;
    g.view = next;
    const RECT area = picture_area();
    InvalidateRect(g.window, &area, FALSE);
}

// ---- the keyboard page -------------------------------------------------------------------------------------------

std::wstring bind_text(const sp::Bind& bind) {
    if (bind.source == sp::Bind::Source::Mouse) {
        static const std::wstring names[] = {"Left mouse button"_w, "Right mouse button"_w, "Middle mouse button"_w,
                                               "Mouse back button"_w, "Mouse forward button"_w};
        return bind.code < std::size(names) ? names[bind.code] : "Mouse button"_w;
    }
    if (bind.source != sp::Bind::Source::Key) return "-"_w;
    const std::string name = sp::key_name(bind.code);
    static const struct {
        const char* from;
        const char* to;
    } friendly[] = {{"LShift", "Left Shift"}, {"RShift", "Right Shift"}, {"LCtrl", "Left Ctrl"},
                    {"RCtrl", "Right Ctrl"},  {"LAlt", "Left Alt"},      {"RAlt", "Right Alt"},
                    {"Up", "Up arrow"},       {"Down", "Down arrow"},    {"Left", "Left arrow"},
                    {"Right", "Right arrow"}};
    for (const auto& f : friendly)
        if (name == f.from) return wide(f.to);
    return wide(name);
}

void refresh_binds() {
    for (std::size_t a = 0; a < sp::kActionCount; ++a)
        for (std::size_t s = 0; s < sp::kMaxBinds; ++s) {
            const int index = static_cast<int>(a * sp::kMaxBinds + s);
            set_text(item(kFirstBind + index),
                     g.key_capture == index ? "Press a key..."_w : bind_text(g.keys.binds[a][s]));
        }
}

void start_key_capture(int index) {
    g.capturing = -1;
    g.status_is_message = false;
    g.key_capture = index;
    g.key_capture_end = GetTickCount() + kCaptureMs;
    refresh_binds();
    refresh_status();
}

void stop_key_capture() {
    g.key_capture = -1;
    refresh_binds();
    refresh_status();
}

void finish_key_capture(sp::Bind bind) {
    const std::size_t action = static_cast<std::size_t>(g.key_capture) / sp::kMaxBinds;
    const std::size_t slot = static_cast<std::size_t>(g.key_capture) % sp::kMaxBinds;
    g.keys.binds[action][slot] = bind;
    stop_key_capture();
    const std::vector<sp::Action> others = sp::bound_actions(g.keys, bind, static_cast<sp::Action>(action));
    if (!others.empty()) {
        std::wstring names;
        for (const sp::Action other : others) {
            if (!names.empty()) names += ", "_w;
            names += kActionLabels[static_cast<std::size_t>(other)];
        }
        show_message(bind_text(bind) + " is also used for: "_w + names + ". Both will happen when you press it."_w);
    }
}

// A key as the game names it: left and right Shift, Ctrl and Alt are different keys.
int key_from_message(const MSG& msg) {
    const int vk = static_cast<int>(msg.wParam);
    const bool extended = (msg.lParam & (1 << 24)) != 0;
    if (vk == VK_SHIFT) {
        const UINT scan = static_cast<UINT>((msg.lParam >> 16) & 0xFF);
        return static_cast<int>(MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX));
    }
    if (vk == VK_CONTROL) return extended ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU) return extended ? VK_RMENU : VK_LMENU;
    return vk;
}

// ---- fields ------------------------------------------------------------------------------------------------------

Field* field_by_id(int id) {
    for (Field& f : g.fields)
        if (f.id == id) return &f;
    return nullptr;
}

bool close_to(double a, double b) { return std::fabs(a - b) < 1e-9; }

std::wstring size_text(double encoded) {
    const long long v = static_cast<long long>(encoded);
    return std::to_wstring(v / 100000) + " x "_w + std::to_wstring(v % 100000);
}
double size_value(int w, int h) { return static_cast<double>(w) * 100000.0 + h; }

// "1920 x 1080", "1920x1080", "1920*1080", "1920 1080".
bool parse_size(const std::wstring& text, int& w, int& h) {
    std::wstring t;
    for (wchar_t c : text) t += (c == L'x' || c == L'X' || c == L'*' || c == 0x00D7 || c == L',') ? L' ' : c;
    wchar_t* end = nullptr;
    const long a = std::wcstol(t.c_str(), &end, 10);
    if (end == t.c_str()) return false;
    const wchar_t* rest = end;
    const long b = std::wcstol(rest, &end, 10);
    if (end == rest) return false;
    while (*end == L' ') ++end;
    if (*end != L'\0' || a <= 0 || b <= 0 || a > 100000 || b > 100000) return false;
    w = static_cast<int>(a);
    h = static_cast<int>(b);
    return true;
}

// A drop-down list as wide as its longest choice, so a narrow box still shows every choice whole when opened.
void fit_drop_width(HWND combo) {
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(combo, WM_GETFONT, 0, 0));
    int widest = 0;
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        const int n = static_cast<int>(SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(i), 0));
        if (n <= 0) continue;
        std::wstring text(static_cast<std::size_t>(n) + 1, L'\0');
        SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(text.data()));
        text.resize(static_cast<std::size_t>(n));
        widest = std::max(widest, static_cast<int>(ui::text_extent(font, text).cx));
    }
    SendMessageW(combo, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(widest + px(16) + GetSystemMetrics(SM_CXVSCROLL)), 0);
}

void refresh_field(Field& f) {
    g.refreshing = true;
    const double v = f.get();
    switch (f.kind) {
        case Kind::Check:
            CheckDlgButton(g.window, f.id, v != 0.0 ? BST_CHECKED : BST_UNCHECKED);
            set_text(f.control, v != 0.0 ? "On"_w : "Off"_w);
            break;
        case Kind::Combo: {
            SendMessageW(f.control, CB_RESETCONTENT, 0, 0);
            int selected = -1;
            for (std::size_t i = 0; i < f.choices.size(); ++i) {
                SendMessageW(f.control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(f.choices[i].label.c_str()));
                if (selected < 0 && close_to(f.choices[i].value, v)) selected = static_cast<int>(i);
            }
            if (selected < 0) {
                const std::wstring extra = (f.show ? f.show(v) : number_text(v)) + " (set in the file)"_w;
                selected = static_cast<int>(SendMessageW(f.control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(extra.c_str())));
            }
            SendMessageW(f.control, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
            fit_drop_width(f.control);
            break;
        }
        case Kind::Slider:
            SendMessageW(f.control, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround(std::clamp(v, f.low, f.high))));
            set_text(f.value, f.show ? f.show(v) : number_text(v));
            break;
        case Kind::Number:
            set_text(f.control, number_text(v));
            g.refused.erase(f.id);
            break;
        case Kind::Size:
            set_text(f.control, size_text(v));
            g.refused.erase(f.id);
            break;
    }
    if (f.enabled) EnableWindow(f.control, f.enabled() ? TRUE : FALSE);
    g.refreshing = false;
}

void refresh_fields() {
    for (Field& f : g.fields) refresh_field(f);
}

void refresh_enabled() {
    for (Field& f : g.fields)
        if (f.enabled) EnableWindow(f.control, f.enabled() ? TRUE : FALSE);
}

// A field changed by the player.
void on_field(Field& f, int notification) {
    if (g.refreshing) return;
    switch (f.kind) {
        case Kind::Check:
            if (notification != BN_CLICKED) return;
            f.set(IsDlgButtonChecked(g.window, f.id) == BST_CHECKED ? 1.0 : 0.0);
            set_text(f.control, f.get() != 0.0 ? "On"_w : "Off"_w);
            break;
        case Kind::Combo: {
            if (notification != CBN_SELCHANGE) return;
            const auto index = static_cast<std::size_t>(SendMessageW(f.control, CB_GETCURSEL, 0, 0));
            if (index < f.choices.size()) f.set(f.choices[index].value);
            break;
        }
        case Kind::Slider:
            f.set(static_cast<double>(SendMessageW(f.control, TBM_GETPOS, 0, 0)));
            set_text(f.value, f.show ? f.show(f.get()) : number_text(f.get()));
            break;
        case Kind::Number: {
            if (notification != EN_CHANGE) return;
            const std::wstring text = window_text(f.control);
            wchar_t* end = nullptr;
            const double v = std::wcstod(text.c_str(), &end);
            while (end != nullptr && *end == L' ') ++end;
            if (text.empty() || end == text.c_str() || *end != L'\0' || !std::isfinite(v) || v < f.low || v > f.high) {
                g.refused[f.id] = f.bad;
            } else {
                g.refused.erase(f.id);
                f.set(v);
            }
            break;
        }
        case Kind::Size: {
            int w = 0, h = 0;
            if (notification == CBN_SELCHANGE) {
                const auto index = static_cast<std::size_t>(SendMessageW(f.control, CB_GETCURSEL, 0, 0));
                if (index >= f.choices.size()) return;
                g.refused.erase(f.id);
                f.set(f.choices[index].value);
                break;
            }
            if (notification != CBN_EDITCHANGE) return;
            if (parse_size(window_text(f.control), w, h) && w >= 320 && h >= 240 && w <= 16384 && h <= 16384) {
                g.refused.erase(f.id);
                f.set(size_value(w, h));
            } else {
                g.refused[f.id] = f.bad;
            }
            break;
        }
    }
    refresh_enabled();
    changed();
}

// ---- building the pages ------------------------------------------------------------------------------------------

// The font of the controls being made: the controller page's at its own scale, the text font elsewhere.
HFONT page_font() { return g.unit_zoom > 0 && g.pad_font != nullptr ? g.pad_font : g.font; }

// A control of the page being built at (x, y), w x h, in units. With g.relayout the control made there before is moved
// instead: build() makes its controls in the same order every time, whatever the width.
HWND make(const wchar_t* cls, const std::wstring& text, DWORD style, int x, int y, int w, int h, int id, HFONT font = nullptr) {
    if (g.relayout) {
        if (g.next_made >= g.made.size()) return nullptr;
        HWND c = g.made[g.next_made++];
        SetWindowPos(c, nullptr, px(g.origin_x + x), px(y) - g.scroll_y, px(w), px(h), SWP_NOZORDER | SWP_NOACTIVATE);
        if (g.building == kPadButtons)  // the controller page's scale may have changed with the window
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font != nullptr ? font : page_font()), FALSE);
        return c;
    }
    HWND c = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | (g.building < 0 ? WS_VISIBLE : 0) | style, px(g.origin_x + x),
                             px(y) - g.scroll_y, px(w), px(h), g.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font != nullptr ? font : page_font()), TRUE);
    g.made.push_back(c);
    if (g.building >= 0) g.page_controls[static_cast<std::size_t>(g.building)].push_back(c);
    if (g.card_open) g.on_card.push_back(c);
    return c;
}

// Moves a control already made, in units.
void move(HWND c, int x, int y, int w, int h) {
    if (c != nullptr)
        SetWindowPos(c, nullptr, px(g.origin_x + x), px(y) - g.scroll_y, px(w), px(h), SWP_NOZORDER | SWP_NOACTIVATE);
}

// A card behind controls of the page being built, in page units (kept in pixels: the controller page has its own scale).
void add_card(int left, int top, int right, int bottom) {
    g.cards[static_cast<std::size_t>(g.building)].push_back(RECT{px(g.origin_x + left), px(top), px(g.origin_x + right), px(bottom)});
}

// How tall `text` is in `font` when wrapped to `width` units, in units.
int text_height(const std::wstring& text, HFONT font, int width) {
    if (text.empty()) return 0;
    return units(ui::text_extent(font, text, px(width)).cy) + 1;
}

constexpr int kHelpId = 900;  // the grey explanations (WM_CTLCOLORSTATIC)

// The page's explanation under its title; its height.
int intro(const std::wstring& text) {
    const int w = g.page_w - 54, h = text_height(text, g.font, w);
    make(L"STATIC", text, SS_LEFT | SS_NOPREFIX, 24, g.top, w, h + 2, kHelpId);
    return h;
}

// One setting on its own card, as in Windows' Settings: the name and its explanation on the left, the control on the
// right; or the control under them when the page is too narrow for both. Returns the card's height.
constexpr int kCardLeft = 18;
int place_card(Field& f, int y) {
    f.id = kFirstField + static_cast<int>(&f - g.fields.data());
    const int right = g.page_w - 12;
    const int inner_l = kCardLeft + 20, inner_r = right - 20, inner_w = inner_r - inner_l;
    int control_w = f.kind == Kind::Check ? 120 : f.kind == Kind::Number ? 110 : 320;
    const bool beside = inner_w - control_w - 32 >= 260;  // the words keep room for about 40 characters a line
    if (!beside) control_w = std::min(control_w, inner_w);
    const int text_w = beside ? inner_w - control_w - 32 : inner_w;
    const int label_h = text_height(f.label, g.font, text_w), help_h = text_height(f.help, g.caption, text_w);
    const int words_h = label_h + (help_h > 0 ? 3 + help_h : 0);
    const int h = beside ? std::max(64, 14 + words_h + 14) : 14 + words_h + 10 + 30 + 12;
    const int control_x = beside ? inner_r - control_w : inner_l;
    const int cy = beside ? y + (h - 30) / 2 : y + 14 + words_h + 10;
    g.card_open = true;
    add_card(kCardLeft, y, right, y + h);
    make(L"STATIC", f.label, SS_LEFT | SS_NOPREFIX, inner_l, y + 14, text_w, label_h, -1);
    if (help_h > 0) make(L"STATIC", f.help, SS_LEFT | SS_NOPREFIX, inner_l, y + 14 + label_h + 3, text_w, help_h, kHelpId, g.caption);
    switch (f.kind) {
        case Kind::Check:
            f.control = make(L"BUTTON", "Off"_w, BS_AUTOCHECKBOX | WS_TABSTOP, beside ? control_x + 34 : control_x, cy,
                             beside ? control_w - 34 : 86, 30, f.id);
            if (!g.relayout) ui::set_switch(f.control);
            break;
        case Kind::Combo:
            f.control = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, control_x, cy + 1, control_w, 300, f.id);
            break;
        case Kind::Slider:
            f.control = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, control_x, cy, control_w - 100, 30, f.id);
            if (!g.relayout)
                SendMessageW(f.control, TBM_SETRANGE, TRUE, MAKELPARAM(static_cast<int>(f.low), static_cast<int>(f.high)));
            f.value = make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, control_x + control_w - 94, cy, 94, 30, -1);
            break;
        case Kind::Number:
            f.control = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, control_x, cy + 2, control_w, 26, f.id);
            break;
        case Kind::Size:
            f.control = make(L"COMBOBOX", L"", CBS_DROPDOWN | WS_TABSTOP | WS_VSCROLL, control_x, cy + 1, control_w, 300, f.id);
            if (!g.relayout) {
                for (const Choice& c : f.choices) SendMessageW(f.control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(c.label.c_str()));
                fit_drop_width(f.control);
            }
            break;
    }
    g.card_open = false;
    return h;
}

// One setting of the keyboard page's right-hand column, `w` units wide: its name with its control beside it, or under
// it when the column is narrow. Returns the height it takes.
int place(Field& f, int x, int y, int w) {
    f.id = kFirstField + static_cast<int>(&f - g.fields.data());
    const int control_w = std::min(f.kind == Kind::Number ? 90 : 220, w);
    const bool beside = w >= 200 + 6 + control_w;
    const int label_w = beside ? w - 6 - control_w : w;
    const int label_h = std::max(20, text_height(f.label, g.font, label_w));
    make(L"STATIC", f.label, SS_LEFT | SS_NOPREFIX, x, beside ? y + 4 : y, label_w, label_h, -1);
    const int cx = beside ? x + w - control_w : x, cy = beside ? y : y + label_h + 4;
    switch (f.kind) {
        case Kind::Check:
            f.control = make(L"BUTTON", "Off"_w, BS_AUTOCHECKBOX | WS_TABSTOP, cx, cy, std::min(120, control_w), 26, f.id);
            if (!g.relayout) ui::set_switch(f.control);
            break;
        case Kind::Combo:
            f.control = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, cx, cy, control_w, 300, f.id);
            break;
        case Kind::Slider:
            f.control = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, cx, cy, control_w - 96, 26, f.id);
            if (!g.relayout)
                SendMessageW(f.control, TBM_SETRANGE, TRUE, MAKELPARAM(static_cast<int>(f.low), static_cast<int>(f.high)));
            f.value = make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, cx + control_w - 90, cy, 90, 26, -1);
            break;
        case Kind::Number:
            f.control = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, cx, cy, control_w, 26, f.id);
            break;
        case Kind::Size:
            f.control = make(L"COMBOBOX", L"", CBS_DROPDOWN | WS_TABSTOP | WS_VSCROLL, cx, cy, control_w, 300, f.id);
            if (!g.relayout)
                for (const Choice& c : f.choices) SendMessageW(f.control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(c.label.c_str()));
            break;
    }
    return beside ? std::max(label_h + 4, 26) + 20 : label_h + 4 + 26 + 16;
}

// The pages' fields, in page order. Each reads and changes g's settings directly.
void add(int page, Kind kind, std::wstring label, std::wstring help, std::function<double()> get,
         std::function<void(double)> set, std::vector<Choice> choices = {}) {
    Field f;
    f.page = page;
    f.kind = kind;
    f.label = std::move(label);
    f.help = std::move(help);
    f.get = std::move(get);
    f.set = std::move(set);
    f.choices = std::move(choices);
    g.fields.push_back(std::move(f));
}

std::wstring percent(double v) { return number_text(v) + " %"_w; }

// The game's own options are -1 until the game or the player sets them.
const Choice kNotChosen{"Not chosen yet (the game decides)"_w, -1.0};

std::vector<Choice> exclusive_modes() {
    std::vector<std::pair<int, int>> modes;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &mode); ++i) {
        const int w = static_cast<int>(mode.dmPelsWidth), h = static_cast<int>(mode.dmPelsHeight);
        if (mode.dmBitsPerPel >= 32 && w >= 640 && h >= 480 &&
            std::find(modes.begin(), modes.end(), std::make_pair(w, h)) == modes.end())
            modes.emplace_back(w, h);
    }
    std::sort(modes.begin(), modes.end(), [](const auto& a, const auto& b) { return a.first * 100000 + a.second > b.first * 100000 + b.second; });
    std::vector<Choice> out{{"The same as the desktop"_w, 0.0}};
    for (const auto& [w, h] : modes) out.push_back({size_text(size_value(w, h)), size_value(w, h)});
    return out;
}

void define_fields() {
    DisplayPrefs& d = g.display;
    OptionPrefs& o = g.options;
    ControlsConfig& k = g.keys;

    // Display
    add(kDisplay, Kind::Combo, "Screen mode"_w,
        "Full screen covers the whole screen at your desktop's resolution. Exclusive full screen switches the monitor "_w
        "to the resolution below. In the game, Alt+Enter switches between window and full screen."_w,
        [&] { return d.fullscreen ? (d.exclusive ? 2.0 : 1.0) : 0.0; },
        [&](double v) {
            d.fullscreen = v != 0.0;
            if (v != 0.0) d.exclusive = v == 2.0;
        },
        {{"Window"_w, 0.0}, {"Full screen"_w, 1.0}, {"Exclusive full screen"_w, 2.0}});
    add(kDisplay, Kind::Size, "Window size"_w,
        "The size of the game's window, as width x height. Pick one or type your own, such as 1600 x 900. You can "_w
        "also drag the window's edge in the game."_w,
        [&] { return size_value(d.width, d.height); },
        [&](double v) {
            const auto e = static_cast<long long>(v);
            d.width = static_cast<int>(e / 100000);
            d.height = static_cast<int>(e % 100000);
        },
        {{"1280 x 720"_w, size_value(1280, 720)},   {"1366 x 768"_w, size_value(1366, 768)},
         {"1600 x 900"_w, size_value(1600, 900)},   {"1920 x 1080"_w, size_value(1920, 1080)},
         {"2560 x 1440"_w, size_value(2560, 1440)}, {"3840 x 2160"_w, size_value(3840, 2160)},
         {"1024 x 768"_w, size_value(1024, 768)},   {"1440 x 1080"_w, size_value(1440, 1080)}});
    g.fields.back().bad = "Window size: type it as width x height, for example 1600 x 900 (at least 320 x 240)."_w;
    add(kDisplay, Kind::Combo, "Full screen resolution"_w,
        "Used only by exclusive full screen. If your monitor cannot show it, close the game and choose another here."_w,
        [&] { return size_value(d.fullscreen_width, d.fullscreen_height); },
        [&](double v) {
            const auto e = static_cast<long long>(v);
            d.fullscreen_width = static_cast<int>(e / 100000);
            d.fullscreen_height = static_cast<int>(e % 100000);
        },
        exclusive_modes());
    g.fields.back().show = [](double v) { return size_text(v); };
    g.fields.back().enabled = [&] { return d.fullscreen && d.exclusive; };
    {
        std::vector<Choice> shapes{{"Automatic (fills the window or screen)"_w, 0.0}};
        for (const gekko::NamedAspect& a : gekko::kNamedAspects) {
            std::wstring label = wide(a.name);
            if (a.w == 4 && a.h == 3) label += " (the original game)"_w;
            shapes.push_back({label, static_cast<double>(a.w) / a.h});
        }
        add(kDisplay, Kind::Combo, "Picture shape"_w,
            "Automatic uses the whole window or screen. A fixed shape adds black bars where it does not fit."_w,
            [&] { return d.aspect; }, [&](double v) { d.aspect = v; }, shapes);
        g.fields.back().show = [](double v) { return wide(gekko::aspect_label(v)); };
    }
    add(kDisplay, Kind::Slider, "Resolution scale"_w,
        "How sharp the 3D picture is drawn. Below 100 % is faster but blurrier; above 100 % is sharper but needs a "_w
        "stronger graphics card."_w,
        [&] { return d.render_scale; }, [&](double v) { d.render_scale = v; });
    g.fields.back().low = 25;
    g.fields.back().high = 200;
    g.fields.back().show = percent;
    add(kDisplay, Kind::Combo, "Edge smoothing (anti-aliasing)"_w,
        "Smooths jagged edges of 3D objects. Higher is smoother but slower. If your graphics card cannot do the "_w
        "chosen level, the game uses the nearest one it can."_w,
        [&] { return static_cast<double>(d.msaa); }, [&](double v) { d.msaa = static_cast<int>(v); },
        {{"Off"_w, 1}, {"2x"_w, 2}, {"4x (default)"_w, 4}, {"8x"_w, 8}, {"16x"_w, 16}, {"32x"_w, 32}});
    add(kDisplay, Kind::Combo, "Texture sharpness (anisotropic filtering)"_w,
        "Keeps ground and wall textures sharp when seen at an angle. Off looks like the original console."_w,
        [&] { return static_cast<double>(d.anisotropy); }, [&](double v) { d.anisotropy = static_cast<int>(v); },
        {{"Off"_w, 1}, {"2x"_w, 2}, {"4x"_w, 4}, {"8x"_w, 8}, {"16x (default)"_w, 16}});
    add(kDisplay, Kind::Slider, "Draw distance"_w,
        "How far away objects are still shown. 1 is the original game; higher shows more but needs more power."_w,
        [&] { return d.draw_distance; }, [&](double v) { d.draw_distance = v; });
    g.fields.back().low = 1;
    g.fields.back().high = 8;
    g.fields.back().show = [](double v) { return number_text(v) + (v == 1.0 ? " (original)"_w : L""); };

    // Frame rate
    add(kFrameRate, Kind::Check, "V-Sync"_w,
        "Waits for your monitor before showing each picture, so the picture never tears. Turn it off only for the "_w
        "lowest delay or if your monitor has G-Sync or FreeSync."_w,
        [&] { return d.vsync ? 1.0 : 0.0; }, [&](double v) { d.vsync = v != 0.0; });
    add(kFrameRate, Kind::Combo, "Frame rate"_w,
        "How many pictures per second are shown. The game itself always runs at its original speed: a higher rate "_w
        "only looks smoother when Smooth motion is on."_w,
        [&] { return d.render_rate; }, [&](double v) { d.render_rate = v; },
        {{"30 per second"_w, 30}, {"60 per second (default)"_w, 60}, {"75 per second"_w, 75}, {"90 per second"_w, 90},
         {"120 per second"_w, 120}, {"144 per second"_w, 144}, {"165 per second"_w, 165}, {"240 per second"_w, 240},
         {"360 per second"_w, 360}, {"Unlimited"_w, 0}});
    g.fields.back().show = [](double v) { return v <= 0.0 ? std::wstring("Unlimited"_w) : number_text(v) + " per second"_w; };
    add(kFrameRate, Kind::Check, "Smooth motion (frame interpolation)"_w,
        "Blends the game's own frames to fill the extra pictures when the frame rate is above 60, for smoother "_w
        "motion. It has no effect at 60 per second or less."_w,
        [&] { return d.frame_interpolation ? 1.0 : 0.0; }, [&](double v) { d.frame_interpolation = v != 0.0; });
    add(kFrameRate, Kind::Check, "Frame counter"_w,
        "Shows the frames per second in the top right corner. In the game, the * key on the number pad switches it."_w,
        [&] { return d.fps_counter ? 1.0 : 0.0; }, [&](double v) { d.fps_counter = v != 0.0; });

    // Sound
    const auto volume = [&](const std::wstring& label, const std::wstring& help, int& value) {
        add(kSound, Kind::Slider, label, help, [&value] { return static_cast<double>(value); },
            [&value](double v) { value = static_cast<int>(v); });
        g.fields.back().low = 0;
        g.fields.back().high = 100;
        g.fields.back().show = percent;
    };
    volume("Master volume"_w, "The volume of everything the game plays."_w, o.master_volume);
    volume("Music"_w, "The music, below the master volume."_w, o.music_volume);
    volume("Sound effects"_w, "Jumps, rings, explosions and other effects, below the master volume."_w, o.sfx_volume);
    volume("Voices"_w, "Spoken lines in cutscenes and during play, below the master volume."_w, o.voice_volume);
    add(kSound, Kind::Combo, "Speaker output"_w,
        "The same as the game's own Options > System. Choose Dolby Pro Logic II only for a surround receiver that "_w
        "decodes it."_w,
        [&] { return static_cast<double>(o.speaker); }, [&](double v) { o.speaker = static_cast<int>(v); },
        {kNotChosen, {"Stereo"_w, 0}, {"Mono"_w, 1}, {"Dolby Pro Logic II"_w, 2}});

    // Language
    add(kLanguage, Kind::Combo, "Spoken language"_w,
        "The language of the voices. Japanese needs the Japanese disc's voice files in a voice_jp folder of the game; "_w
        "without them the voices stay English."_w,
        [&] { return static_cast<double>(o.voice); }, [&](double v) { o.voice = static_cast<int>(v); },
        {kNotChosen, {"English"_w, 1}, {"Japanese"_w, 0}});
    add(kLanguage, Kind::Check, "Subtitles"_w,
        "Shows the text of spoken lines. Off hides only the text; the voices still play."_w,
        [&] { return o.subtitles ? 1.0 : 0.0; }, [&](double v) { o.subtitles = v != 0.0; });
    add(kLanguage, Kind::Combo, "Text language"_w,
        "The language of menus and subtitles. The same as the game's own Options > Subtitle."_w,
        [&] { return static_cast<double>(o.language); }, [&](double v) { o.language = static_cast<int>(v); },
        {kNotChosen, {"English"_w, 1}, {"French"_w, 2}, {"German"_w, 4}, {"Italian"_w, 5}, {"Spanish"_w, 3}, {"Japanese"_w, 0}});

    // Controller options
    add(kPadOptions, Kind::Combo, "Vibration"_w,
        "Whether the controller rumbles. The same as the game's own Options > Vibration."_w,
        [&] { return static_cast<double>(o.vibration); }, [&](double v) { o.vibration = static_cast<int>(v); },
        {kNotChosen, {"On"_w, 1}, {"Off"_w, 0}});
    add(kPadOptions, Kind::Slider, "Rumble strength"_w,
        "How hard the controller rumbles when vibration is on. 100 % is the original strength."_w,
        [&] { return static_cast<double>(o.rumble_strength); }, [&](double v) { o.rumble_strength = static_cast<int>(v); });
    g.fields.back().low = 25;
    g.fields.back().high = 100;
    g.fields.back().show = percent;
    add(kPadOptions, Kind::Check, "Invert camera left and right"_w,
        "Pushing the camera stick right turns the camera the other way."_w,
        [&] { return o.invert_camera_x ? 1.0 : 0.0; }, [&](double v) { o.invert_camera_x = v != 0.0; });
    add(kPadOptions, Kind::Check, "Invert camera up and down"_w,
        "Pushing the camera stick up moves the camera the other way."_w,
        [&] { return o.invert_camera_y ? 1.0 : 0.0; }, [&](double v) { o.invert_camera_y = v != 0.0; });

    // Keyboard and mouse (the right-hand column of the page)
    const auto speed = [&](const std::wstring& label, double& value) {
        add(kKeyboard, Kind::Number, label, L"", [&value] { return value; }, [&value](double v) { value = v; });
        g.fields.back().low = 0.01;
        g.fields.back().high = 50;
        g.fields.back().bad = "Mouse speed must be a number from 0.01 to 50, for example 1.5."_w;
    };
    speed("Mouse speed, left and right"_w, k.mouse_sensitivity_x);
    speed("Mouse speed, up and down"_w, k.mouse_sensitivity_y);
    add(kKeyboard, Kind::Check, "Invert mouse left and right"_w, L"", [&] { return k.mouse_invert_x ? 1.0 : 0.0; },
        [&](double v) { k.mouse_invert_x = v != 0.0; });
    add(kKeyboard, Kind::Check, "Invert mouse up and down"_w, L"", [&] { return k.mouse_invert_y ? 1.0 : 0.0; },
        [&](double v) { k.mouse_invert_y = v != 0.0; });
    add(kKeyboard, Kind::Check, "Keep the pointer in the game"_w, L"", [&] { return k.mouse_capture ? 1.0 : 0.0; },
        [&](double v) { k.mouse_capture = v != 0.0; });
    add(kKeyboard, Kind::Slider, "Walk speed"_w, L"", [&] { return static_cast<double>(k.walk_deflection); },
        [&](double v) { k.walk_deflection = static_cast<int>(v); });
    g.fields.back().low = 1;
    g.fields.back().high = 100;
    g.fields.back().show = [](double v) {
        return number_text(v) + (v == ControlsConfig::kDefaultWalkDeflection ? " (default)"_w : L"");
    };

    // Saving
    add(kSaving, Kind::Combo, "Autosave"_w,
        "On: the game saves by itself after stages and when you leave its Options screen. Off: it saves only when "_w
        "you choose to."_w,
        [&] { return static_cast<double>(o.autosave); }, [&](double v) { o.autosave = static_cast<int>(v); },
        {kNotChosen, {"On"_w, 1}, {"Off"_w, 0}});
}

// The controller buttons page at a width and from a top in its own units: the list of buttons on a card, and the
// pictures right of it, or under it when the page is too narrow for both. Returns where the page ends.
int lay_out_pad_page(int page_w, int top, HFONT font) {
    const bool beside = page_w >= 414 + 360 + 12;
    const int card_r = beside ? 400 : page_w - 12;
    constexpr int kLeft = 28, kNameW = 120, kDotW = 24;
    const int inner_w = card_r - 16 - kLeft;
    const bool name_beside = inner_w >= 352;  // each row's name beside its button
    const int button_w = name_beside ? std::min(200, inner_w - kNameW - 2 - 6 - kDotW) : inner_w - 6 - kDotW;
    int y = top + 12;
    g.card_open = true;
    const std::wstring head = "Game button"_w, column = "Your controller (click to change)"_w;
    if (name_beside) {
        const int column_h = std::max(20, text_height(column, font, inner_w - kNameW - 2));
        make(L"STATIC", head, SS_LEFT, kLeft, y, kNameW, 20, -1);
        make(L"STATIC", column, SS_LEFT, kLeft + kNameW + 2, y, inner_w - kNameW - 2, column_h, -1);
        y += column_h + 6;
    } else {
        const int head_h = std::max(20, text_height(head, font, inner_w));
        const int column_h = std::max(20, text_height(column, font, inner_w));
        make(L"STATIC", head, SS_LEFT, kLeft, y, inner_w, head_h, -1);
        make(L"STATIC", column, SS_LEFT, kLeft, y + head_h + 2, inner_w, column_h, -1);
        y += head_h + 2 + column_h + 6;
    }
    for (int i = 0; i < static_cast<int>(kControls.size()); ++i) {
        const std::wstring name = wide(kControls[static_cast<std::size_t>(i)].name);
        if (name_beside) {
            make(L"STATIC", name, SS_LEFT | SS_CENTERIMAGE, kLeft, y, kNameW, 26, -1);
        } else {
            make(L"STATIC", name, SS_LEFT | SS_CENTERIMAGE, kLeft, y, inner_w, 22, -1);
            y += 22;
        }
        const int button_x = name_beside ? kLeft + kNameW + 2 : kLeft;
        make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, button_x, y, button_w, 26, kFirstRow + i);
        make(L"STATIC", L"", SS_CENTER | SS_CENTERIMAGE, button_x + button_w + 6, y, kDotW, 26, kFirstLive + i);
        y += name_beside ? 30 : 34;
    }
    y += 8;
    const std::wstring trigger = "Trigger press needed"_w, prompts = "Button prompts"_w;
    if (name_beside) {
        make(L"STATIC", trigger, SS_LEFT | SS_CENTERIMAGE, kLeft, y, 140, 30, -1);
    } else {
        make(L"STATIC", trigger, SS_LEFT | SS_CENTERIMAGE, kLeft, y, inner_w, 22, -1);
        y += 22;
    }
    const int slider_x = name_beside ? kLeft + 142 : kLeft;
    const int slider_w = name_beside ? std::min(180, inner_w - 142 - 42) : inner_w - 42;
    HWND slider = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, slider_x, y, slider_w, 30, kThreshold);
    if (!g.relayout) SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 255));
    make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, slider_x + slider_w + 6, y, 36, 30, kThresholdValue);
    y += 34;
    if (name_beside) {
        make(L"STATIC", prompts, SS_LEFT | SS_CENTERIMAGE, kLeft, y, kNameW, 26, -1);
    } else {
        make(L"STATIC", prompts, SS_LEFT | SS_CENTERIMAGE, kLeft, y, inner_w, 22, -1);
        y += 22;
    }
    HWND combo = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, name_beside ? kLeft + kNameW + 2 : kLeft, y,
                      std::min(200, name_beside ? inner_w - kNameW - 2 : inner_w), 200, kStyle);
    if (!g.relayout) {
        for (const auto& s : kStyles) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.label.c_str()));
        fit_drop_width(combo);
    }
    y += 34;
    const std::wstring swap = "Move with the right stick, camera with the left"_w;
    const int swap_h = std::max(26, text_height(swap, font, inner_w - 26) + 6);
    make(L"BUTTON", swap, BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP, kLeft, y, inner_w, swap_h, kSwapSticks);
    y += swap_h;
    g.card_open = false;
    add_card(12, top, card_r, y + 14);
    // The pictures keep their shape: as wide as the room allows, up to their full size.
    const int picture_x = beside ? 414 : 12;
    const int picture_w = std::max(1, std::min(kPictureW, page_w - 12 - picture_x));
    const int picture_y = beside ? top + 4 : y + 14 + 16;
    g.picture = RECT{px(g.origin_x + picture_x), px(picture_y), px(g.origin_x + picture_x + picture_w), px(picture_y + picture_w * kPictureH / kPictureW)};
    return std::max(y + 14, picture_y + picture_w * kPictureH / kPictureW);
}

// The controller buttons page has a scale of its own: laid out as at zoom 100 % (the list and the pictures side by side,
// kPadPageW x kPadPageH units), it grows with the zoom and shrinks to fit the window, the list and the pictures together,
// like a picture. Only when that would make it smaller than kSmallestPadZoom does it use the text zoom and put the
// pictures under the list. Returns where the page ends, in the text zoom's units.
constexpr int kPadPageW = 1094, kPadPageH = 600, kSmallestPadZoom = 75;
int build_pad_page() {
    RECT client;
    GetClientRect(g.window, &client);
    const int width_fit = MulDiv(px(g.page_w), 96 * 100, g.dpi * kPadPageW);
    // The height above the bottom bar (as last laid out), so that the page and the Save button fit when the window can.
    const int height_fit = MulDiv(std::max(1, static_cast<int>(client.bottom) - px(g.top) - px(g.bar_h) - px(16)), 96 * 100,
                                  g.dpi * kPadPageH);
    const int text = g.look.text_size;
    if (width_fit < kSmallestPadZoom) return lay_out_pad_page(g.page_w, g.top, g.font);
    const int zoom = std::max(kSmallestPadZoom, std::min({text, width_fit, std::max(height_fit, kSmallestPadZoom)}));
    HFONT old_font = nullptr;
    if (g.pad_font == nullptr || g.pad_font_zoom != zoom) {
        old_font = g.pad_font;
        g.pad_font = CreateFontW(ui::font_height(10, g.dpi, zoom), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                 CLEARTYPE_QUALITY, 0, ui::text_face());
        g.pad_font_zoom = zoom;
    }
    // In the page's own units: the same pixels as the text zoom's page_x, top and page_w.
    g.unit_zoom = zoom;
    g.origin_x = MulDiv(g.page_x, text, zoom);
    const int end = lay_out_pad_page(MulDiv(g.page_w, text, zoom), MulDiv(g.top, text, zoom), g.pad_font);
    g.unit_zoom = 0;
    g.origin_x = g.page_x;
    if (old_font != nullptr) DeleteObject(old_font);  // its controls have the new one now
    return MulDiv(end, zoom, text) + 1;
}

// The keyboard page: the actions and their keys on one card, the mouse settings on another, beside it or under it.
// Returns where the page ends.
int build_keyboard_page() {
    constexpr int kLeft = 26;
    const int slots = static_cast<int>(sp::kMaxBinds);
    // The boxes as wide as the longest key name they can show, the names as wide as the longest action's.
    int key_w = units(ui::text_extent(g.font, "Press a key..."_w).cx);
    for (int b = 0; b < 5; ++b)
        key_w = std::max(key_w, units(ui::text_extent(g.font, bind_text(sp::mouse_bind(static_cast<sp::MouseButton>(b)))).cx));
    key_w += 28;  // the button's own edges
    int label_w = 0;
    for (const std::wstring& label : kActionLabels) label_w = std::max(label_w, units(ui::text_extent(g.font, label).cx) + 8);
    const int full_table = kLeft + label_w + 2 + slots * (key_w + 4) + 12;  // from the card's left edge at 12
    const bool beside = g.page_w >= 12 + full_table + 12 + 440 + 12;
    const int table_r = beside ? 12 + full_table : g.page_w - 12;
    const int inner_w = table_r - 12 - kLeft;
    // Each action's name beside its boxes; or over them, in a row; or over them, one under another.
    const bool label_beside = inner_w >= label_w + 2 + slots * (key_w + 4);
    const bool one_row = label_beside || inner_w >= slots * (key_w + 4);
    const int bind_w = label_beside ? std::min(key_w + 32, (inner_w - label_w - 2) / slots - 4)
                       : one_row    ? std::min(key_w + 32, (inner_w - (slots - 1) * 4) / slots)
                                    : inner_w;
    const int kLabelW = label_w;
    int y = g.top + 12;
    g.card_open = true;
    const std::wstring head = "Action"_w, hint = "Click a box, then press a key or a mouse button"_w;
    if (label_beside) {
        const int hint_h = std::max(20, text_height(hint, g.font, inner_w - kLabelW - 2));
        make(L"STATIC", head, SS_LEFT, kLeft, y, kLabelW - 8, 20, -1);
        make(L"STATIC", hint, SS_LEFT | SS_NOPREFIX, kLeft + kLabelW + 2, y, inner_w - kLabelW - 2, hint_h, -1);
        y += hint_h + 6;
    } else {
        const int hint_h = std::max(20, text_height(hint, g.font, inner_w));
        make(L"STATIC", head, SS_LEFT, kLeft, y, inner_w, 20, -1);
        make(L"STATIC", hint, SS_LEFT | SS_NOPREFIX, kLeft, y + 22, inner_w, hint_h, -1);
        y += 22 + hint_h + 6;
    }
    for (std::size_t a = 0; a < sp::kActionCount; ++a) {
        if (label_beside) {
            make(L"STATIC", kActionLabels[a], SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, kLeft, y, kLabelW, 22, -1);
        } else {
            make(L"STATIC", kActionLabels[a], SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, kLeft, y, inner_w, 22, -1);
            y += 23;
        }
        for (int s = 0; s < slots; ++s) {
            const int x = label_beside ? kLeft + kLabelW + 2 + s * (bind_w + 4) : one_row ? kLeft + s * (bind_w + 4) : kLeft;
            make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, x, y, bind_w, 22,
                 kFirstBind + static_cast<int>(a * sp::kMaxBinds) + s);
            if (!one_row && s + 1 < slots) y += 26;
        }
        y += label_beside ? 24 : 30;
    }
    add_card(12, g.top, table_r, y + 12);
    const int column_l = beside ? table_r + 12 : 12, column_top = beside ? g.top : y + 12 + 16, column_r = g.page_w - 12;
    const int x = column_l + 18, w = column_r - 20 - x;
    y = column_top + 16;
    for (Field& f : g.fields)
        if (f.page == kKeyboard) y += place(f, x, y, w);
    const std::wstring note =
        "Mouse speed 1 is the normal speed; 2 turns twice as fast. Walk speed is how fast you move while holding "_w
        "Walk (36 is the default). Keyboard and mouse changes also apply when you switch back to a running game."_w;
    const int note_h = text_height(note, g.caption, w);
    make(L"STATIC", note, SS_LEFT | SS_NOPREFIX, x, y + 6, w, note_h, kHelpId, g.caption);
    g.card_open = false;
    add_card(column_l, column_top, column_r, y + 6 + note_h + 16);
    return y + 6 + note_h + 16;
}

// The zoom and theme choices, as offered under the navigation list.
const int kTextSizes[] = {100, 125, 150, 175, 200};
const struct {
    std::wstring label;
    ui::Theme theme;
} kThemes[] = {{"Theme: same as Windows"_w, ui::Theme::System},
               {"Theme: light"_w, ui::Theme::Light},
               {"Theme: dark"_w, ui::Theme::Dark},
               {"Theme: Shadow"_w, ui::Theme::Shadow}};

// The navigation list's icons (Segoe Fluent Icons / Segoe MDL2 Assets), in page order.
const wchar_t kPageIcons[kPageCount] = {0xE7F4 /*monitor*/, 0xE916 /*stopwatch*/, 0xE767 /*volume*/, 0xE774 /*globe*/,
                                        0xE7FC /*game*/,    0xE713 /*setting*/,   0xE765 /*keyboard*/, 0xE74E /*save*/};

// The width the window gives the layout, in units. The scroll bar's room is kept whether it shows or not, so the bar
// coming or going never changes the layout (which could make it come and go for ever).
int layout_units() {
    RECT client;
    GetClientRect(g.window, &client);
    int w = client.right;
    if ((GetWindowLongW(g.window, GWL_STYLE) & WS_VSCROLL) == 0) w -= GetSystemMetrics(SM_CXVSCROLL);
    return units(std::max(w, 1));
}

// The navigation column and the page's width, for the window's width.
void choose_columns() {
    g.layout_w = layout_units();
    RECT client;
    GetClientRect(g.window, &client);
    g.layout_h = client.bottom;
    const int total = std::max(g.layout_w, kNarrowNavW - 8 + kSmallestPage);
    g.narrow_nav = *ui::icon_face() != L'\0' && total < kNavW - 8 + 520;
    g.page_x = (g.narrow_nav ? kNarrowNavW : kNavW) - 8;
    // Without an icon font the navigation keeps its words, and the page takes what is left, however narrow: it must
    // never run past the window's side.
    g.page_w = g.narrow_nav || *ui::icon_face() != L'\0' ? std::max(kSmallestPage, total - g.page_x) : std::max(120, total - g.page_x);
}

// The bar at the bottom, on every page: what is going on, on the left, and the buttons on the right; or the buttons
// under it when the page is narrow (in as many rows as they need). With the narrow navigation list, the zoom and theme
// are here too.
void build_bar(HWND sizes, HWND themes) {
    const int b = g.bottom, left = 22, avail = g.page_w - 44;
    HWND status = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, left, b + 10, avail, 22, kStatus);
    HWND info = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, left, b + 34, avail, 34, kInfo, g.caption);
    HWND buttons[] = {make(L"BUTTON", "Restore defaults"_w, BS_PUSHBUTTON | WS_TABSTOP, left, b, 176, 34, kRestore),
                      make(L"BUTTON", "Save"_w, BS_DEFPUSHBUTTON | WS_TABSTOP, left, b, 120, 34, kSave),
                      make(L"BUTTON", "Close"_w, BS_PUSHBUTTON | WS_TABSTOP, left, b, 118, 34, kClose)};
    if (!g.relayout) {
        ui::set_icon(buttons[0], 0xE777 /*restore*/);
        ui::set_icon(buttons[1], 0xE74E /*save*/);
        ui::set_primary(buttons[1]);
    }
    int widths[std::size(buttons)] = {};
    int all = 0;
    for (std::size_t i = 0; i < std::size(buttons); ++i) {
        widths[i] = std::min(avail, std::max(96, units(ui::button_width(buttons[i])) + 2));
        all += widths[i] + (i > 0 ? 8 : 0);
    }
    const bool beside = avail - all - 24 >= 300;
    g.status_w = beside ? avail - all - 24 : avail;
    g.status_h = std::max(22, text_height(window_text(status), g.font, g.status_w));
    g.info_h = std::max(18, text_height(window_text(info), g.caption, g.status_w));
    move(status, left, b + 10, g.status_w, g.status_h);
    move(info, left, b + 10 + g.status_h + 2, g.status_w, g.info_h);
    int bottom = b + 10 + g.status_h + 2 + g.info_h;
    // The buttons, right-aligned, a row at a time.
    int y = beside ? b + 16 : bottom + 10;
    for (std::size_t first = 0; first < std::size(buttons);) {
        std::size_t end = first;
        int row_w = 0;
        while (end < std::size(buttons) && (end == first || row_w + 8 + widths[end] <= avail)) {
            row_w += (end > first ? 8 : 0) + widths[end];
            ++end;
        }
        int x = left + avail - row_w;
        for (std::size_t i = first; i < end; ++i) {
            move(buttons[i], x, y, widths[i], 34);
            x += widths[i] + 8;
        }
        bottom = std::max(bottom, y + 34);
        y += 42;
        first = end;
    }
    if (g.narrow_nav) {
        y = bottom + 12;
        const int look_w = std::min(kLookW, avail);
        if (2 * look_w + 8 <= avail) {
            move(sizes, left, y, look_w, 300);
            move(themes, left + look_w + 8, y, look_w, 300);
            bottom = y + 30;
        } else {
            move(sizes, left, y, look_w, 300);
            move(themes, left, y + 38, look_w, 300);
            bottom = y + 38 + 30;
        }
    }
    g.bar_h = bottom - b + 18;
}

void build() {
    // The navigation column: the pages, then the zoom and theme.
    g.origin_x = 0;
    for (auto& cards : g.cards) cards.clear();
    const int nav_w = g.narrow_nav ? kNarrowNavW : kNavW;
    g.tabs = make(L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
                  12, 16, nav_w - 24, kPageCount * kNavItemH + 4, kTabs);
    const int look_y = 16 + kPageCount * kNavItemH + 24;
    HWND sizes = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 16, look_y, kLookW, 300, kTextSize);
    HWND themes = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 16, look_y + 40, kLookW, 300, kColours);
    if (!g.relayout) {
        for (int i = 0; i < kPageCount; ++i) SendMessageW(g.tabs, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kPageNames[i].c_str()));
        SendMessageW(g.tabs, LB_SETITEMHEIGHT, 0, px(kNavItemH));
        for (const int size : kTextSizes) {
            const std::wstring label = "Zoom: "_w + std::to_wstring(size) + " %"_w;
            SendMessageW(sizes, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        for (const auto& t : kThemes) SendMessageW(themes, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.label.c_str()));
        fit_drop_width(sizes);
        fit_drop_width(themes);
    }

    // The pages, right of it, under a title that wraps when the page is narrow.
    g.origin_x = g.page_x;
    const int title_w = g.page_w - 44;
    int title_h = 0;
    for (const std::wstring& name : kPageNames) title_h = std::max(title_h, text_height(name, g.title, title_w));
    make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 22, 14, title_w, title_h, kPageTitle, g.title);
    g.top = std::max(72, 14 + title_h + 16);
    if (!g.relayout) {
        g.on_card.clear();
        define_fields();
    }
    const std::wstring intros[kPageCount] = {
        "How the game's window and picture look."_w,
        "How smoothly the picture is shown. The game itself always runs at its original speed."_w,
        "The volume of each kind of sound."_w,
        "Voices, subtitles and the language of the game's text."_w,
        {},
        "Rumble and the camera stick. Which button does what, the trigger press and the button prompts are on the "_w
        "Controller buttons page."_w,
        {},
        "Whether the game saves your progress by itself."_w};
    g.bottom = 0;
    for (int p = 0; p < kPageCount; ++p) {
        g.building = p;
        if (p == kPadButtons) {
            g.bottom = std::max(g.bottom, build_pad_page() + 12);
            continue;
        }
        if (p == kKeyboard) {
            g.bottom = std::max(g.bottom, build_keyboard_page() + 12);
            continue;
        }
        int y = g.top + intro(intros[p]) + 14;
        for (Field& f : g.fields) {
            if (f.page != p) continue;
            y += place_card(f, y) + 4;
        }
        g.bottom = std::max(g.bottom, y + 12);
    }
    for (const auto& cards : g.cards)
        for (const RECT& r : cards) g.bottom = std::max(g.bottom, units(r.bottom) + 12);
    g.building = -1;
    build_bar(sizes, themes);
    g.origin_x = 0;
}

void show_page(int page) {
    g.capturing = -1;
    g.key_capture = -1;
    g.page = page;
    for (int p = 0; p < kPageCount; ++p)
        for (HWND h : g.page_controls[static_cast<std::size_t>(p)]) ShowWindow(h, p == page ? SW_SHOW : SW_HIDE);
    if (static_cast<int>(SendMessageW(g.tabs, LB_GETCURSEL, 0, 0)) != page)
        SendMessageW(g.tabs, LB_SETCURSEL, static_cast<WPARAM>(page), 0);
    set_text(item(kPageTitle), kPageNames[page]);
    InvalidateRect(g.window, nullptr, TRUE);  // the page's cards
    refresh_rows();
    refresh_binds();
    refresh_status();
}

// ---- defaults, saving --------------------------------------------------------------------------------------------

void restore_defaults() {
    const DisplayPrefs d;
    const OptionPrefs o;
    const ControlsConfig k = sp::default_controls();
    switch (g.page) {
        case kDisplay:
            g.display.fullscreen = d.fullscreen;
            g.display.exclusive = d.exclusive;
            g.display.width = d.width;
            g.display.height = d.height;
            g.display.fullscreen_width = d.fullscreen_width;
            g.display.fullscreen_height = d.fullscreen_height;
            g.display.aspect = d.aspect;
            g.display.render_scale = d.render_scale;
            g.display.msaa = d.msaa;
            g.display.anisotropy = d.anisotropy;
            g.display.draw_distance = d.draw_distance;
            break;
        case kFrameRate:
            g.display.vsync = d.vsync;
            g.display.render_rate = d.render_rate;
            g.display.frame_interpolation = d.frame_interpolation;
            g.display.fps_counter = d.fps_counter;
            break;
        case kSound:
            g.options.master_volume = o.master_volume;
            g.options.music_volume = o.music_volume;
            g.options.sfx_volume = o.sfx_volume;
            g.options.voice_volume = o.voice_volume;
            g.options.speaker = o.speaker;
            break;
        case kLanguage:
            g.options.voice = o.voice;
            g.options.subtitles = o.subtitles;
            g.options.language = o.language;
            break;
        case kPadButtons:
            g.binding = GamepadBinding{};
            break;
        case kPadOptions:
            g.options.vibration = o.vibration;
            g.options.rumble_strength = o.rumble_strength;
            g.options.invert_camera_x = o.invert_camera_x;
            g.options.invert_camera_y = o.invert_camera_y;
            break;
        case kKeyboard:
            g.keys = k;
            break;
        case kSaving:
            g.options.autosave = o.autosave;
            break;
        default:
            break;
    }
    g.capturing = -1;
    g.key_capture = -1;
    refresh_fields();
    refresh_rows();
    refresh_pad_options();
    refresh_binds();
    show_message("This page is back to the game's defaults. Click Save to keep them."_w);
}

bool save() {
    if (!g.refused.empty()) {
        show_message(g.refused.begin()->second + " Nothing was saved."_w);
        return false;
    }
    for (const std::string& problem : {check_display(g.display), check_options(g.options), check_controls(g.keys)})
        if (!problem.empty()) {
            show_message(wide(problem) + " Nothing was saved."_w);
            return false;
        }
    std::string error;
    std::vector<std::wstring> saved;
    const auto fail = [&](const std::wstring& file) {
        std::wstring text = file + " was not saved: "_w + wide(error);
        if (!saved.empty()) {
            text += " (Saved:"_w;
            for (const std::wstring& s : saved) text += " "_w + s;
            text += ".)"_w;
        }
        show_message(text);
        return false;
    };
    if (!(g.display == g.display_saved)) {
        if (!save_display(g.folder / "display.cfg", g.display_saved, g.display, &error)) return fail("display.cfg"_w);
        g.display_saved = g.display;
        saved.push_back("display.cfg"_w);
    }
    if (!(g.options == g.options_saved)) {
        if (!save_options(g.folder / "options.cfg", g.options_saved, g.options, &error)) return fail("options.cfg"_w);
        g.options_saved = g.options;
        saved.push_back("options.cfg"_w);
    }
    if (!same(g.binding, g.saved)) {
        if (!save_binding(g.folder / "pad.cfg", g.binding, &error)) return fail("pad.cfg"_w);
        g.saved = g.binding;
        saved.push_back("pad.cfg"_w);
    }
    if (!(g.keys == g.keys_saved)) {
        if (!save_controls(g.folder / "controls.cfg", g.keys_saved, g.keys, &error)) return fail("controls.cfg"_w);
        g.keys_saved = g.keys;
        saved.push_back("controls.cfg"_w);
    }
    show_message(saved.empty() ? "Nothing to save: no setting was changed."_w
                               : "Saved. The game uses the new settings the next time it starts."_w);
    return true;
}

bool confirm_close() {
    if (!unsaved() && g.refused.empty()) return true;
    const int answer = MessageBoxW(g.window, "Save your changes before closing?"_w.c_str(), kTitle.c_str(), MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL) return false;
    return answer == IDNO || save();
}

// ---- zoom and theme ---------------------------------------------------------------------------------------------

// The fonts of Windows 11's type ramp: body text, captions (the explanations), the page title, and the icons.
void make_font() {
    for (HFONT* f : {&g.font, &g.caption, &g.title, &g.icons})
        if (*f != nullptr) DeleteObject(*f);
    const auto font = [](int points, int weight, const wchar_t* face) {
        return CreateFontW(ui::font_height(points, g.dpi, g.look.text_size), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, face);
    };
    g.font = font(10, FW_NORMAL, ui::text_face());
    g.caption = font(9, FW_NORMAL, ui::text_face());
    g.title = font(20, FW_SEMIBOLD, ui::title_face());
    g.icons = *ui::icon_face() != L'\0' ? font(12, FW_NORMAL, ui::icon_face()) : nullptr;
    if (g.pad_font != nullptr) DeleteObject(g.pad_font);  // the controller page makes its own again (build_pad_page())
    g.pad_font = nullptr;
    g.pad_font_zoom = 0;
}

void refresh_look() {
    for (int i = 0; i < static_cast<int>(std::size(kTextSizes)); ++i)
        if (kTextSizes[i] == g.look.text_size) SendMessageW(item(kTextSize), CB_SETCURSEL, static_cast<WPARAM>(i), 0);
    for (int i = 0; i < static_cast<int>(std::size(kThemes)); ++i)
        if (kThemes[i].theme == g.look.theme) SendMessageW(item(kColours), CB_SETCURSEL, static_cast<WPARAM>(i), 0);
}

constexpr DWORD kStyleFlags = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME | WS_CLIPCHILDREN;

// The window's smallest size: small enough for any screen, the same at every zoom (the page scrolls).
SIZE smallest_window() {
    // 760 pixels at 96 dpi: the longest drop-down choice still fits at zoom 200 %.
    return SIZE{MulDiv(760, g.dpi, 96), MulDiv(480, g.dpi, 96)};
}

// The window at the start: as wide as the pages at zoom 100 % and as tall as the content was then (`content_h`
// pixels), but no larger than the screen it is on. Zoom never changes it afterwards.
void size_window_at_start(int content_h) {
    RECT want{0, 0, MulDiv(kClientW, g.dpi, 96) + GetSystemMetrics(SM_CXVSCROLL), content_h};
    AdjustWindowRect(&want, kStyleFlags & ~WS_CLIPCHILDREN, FALSE);
    int w = want.right - want.left, h = want.bottom - want.top;
    RECT now;
    GetWindowRect(g.window, &now);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(g.window, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT& work = monitor.rcWork;
    w = std::min(w, static_cast<int>(work.right - work.left));
    h = std::min(h, static_cast<int>(work.bottom - work.top));
    const int x = std::clamp(static_cast<int>(now.left), static_cast<int>(work.left), static_cast<int>(work.right) - w);
    const int y = std::clamp(static_cast<int>(now.top), static_cast<int>(work.top), static_cast<int>(work.bottom) - h);
    SetWindowPos(g.window, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void rebuild_view(int focus);

// Lays the controls out again for the window's present width: they are moved, not made again.
void relayout() {
    if (g.window == nullptr || g.made.empty() || g.relayout || g.rebuilding) return;
    choose_columns();
    SendMessageW(g.window, WM_SETREDRAW, FALSE, 0);
    g.relayout = true;
    g.next_made = 0;
    build();
    g.relayout = false;
    const bool whole = g.next_made == g.made.size();
    SendMessageW(g.window, WM_SETREDRAW, TRUE, 0);
    if (!whole) {  // build() made other controls than before (it must not): make them all again
        rebuild_view(0);
        return;
    }
    update_scrollbars();
    RedrawWindow(g.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

// The status lines changed: laid out again when they need more or fewer lines than they have.
void fit_status() {
    if (g.window == nullptr || g.made.empty() || g.relayout || g.rebuilding || g.status_w <= 0) return;
    const int status_h = std::max(22, text_height(window_text(item(kStatus)), g.font, g.status_w));
    const int info_h = std::max(18, text_height(window_text(item(kInfo)), g.caption, g.status_w));
    if (status_h != g.status_h || info_h != g.info_h) relayout();
}

// Makes every control again at the chosen text size and in the chosen colours, laid out for the window's width. The
// settings being edited are kept; `focus` (a control id) gets the keyboard focus afterwards.
void rebuild_view(int focus) {
    const int page = g.page;
    if (focus == 0) {  // the control that has the focus now
        HWND f = GetFocus();
        while (f != nullptr && GetParent(f) != g.window) f = GetParent(f);
        if (f != nullptr) focus = GetDlgCtrlID(f);
    }
    SendMessageW(g.window, WM_SETREDRAW, FALSE, 0);
    g.rebuilding = true;
    for (HWND c = GetWindow(g.window, GW_CHILD); c != nullptr; c = GetWindow(g.window, GW_CHILD)) DestroyWindow(c);
    for (auto& controls : g.page_controls) controls.clear();
    g.on_card.clear();
    g.made.clear();
    g.fields.clear();
    g.refused.clear();
    g.scroll_y = 0;
    g.last_focus = nullptr;
    ui::set_dark(ui::is_dark(g.look.theme), g.look.theme == ui::Theme::Shadow);
    make_font();
    choose_columns();
    build();
    refresh_fields();
    refresh_rows();
    refresh_pad_options();
    refresh_binds();
    refresh_look();
    ui::apply_to_window(g.window);
    show_page(page);
    g.rebuilding = false;
    SendMessageW(g.window, WM_SETREDRAW, TRUE, 0);
    fit_status();  // the status lines' text came after the layout: laid out again if it needs other room
    update_scrollbars();
    if (focus > 0 && item(focus) != nullptr) SetFocus(item(focus));
    RedrawWindow(g.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

// A new text size or colour choice: kept in appearance.cfg (the Mod Manager uses it too) and shown at once, in the
// window as it is (zoom lays the pages out again; it never resizes the window).
void set_look(ui::Appearance next, int focus) {
    next.text_size = ui::step_text_size(next.text_size, 0);
    if (next == g.look) return;
    const bool size_changed = next.text_size != g.look.text_size;
    g.look = next;
    const bool stored = g.checking || ui::save_appearance(g.folder, g.look);
    rebuild_view(focus);
    if (!stored)
        show_message("The zoom and theme could not be stored in appearance.cfg, so they are used only until this "_w
                     "window closes."_w);
    else if (size_changed)
        show_message("Zoom "_w + std::to_wstring(g.look.text_size) +
                     " %. Ctrl and + or - also zoom in and out, and Ctrl and 0 goes back to 100 %."_w);
}

// A zoom or theme list's choice is used when the list closes, or at once when it is changed while closed (arrow keys
// on the closed list): not while the player moves through the open list, which a rebuild would close.
bool look_chosen(HWND list, int code) {
    return code == CBN_CLOSEUP || (code == CBN_SELCHANGE && SendMessageW(list, CB_GETDROPPEDSTATE, 0, 0) == FALSE);
}

void step_text(int steps) {
    ui::Appearance next = g.look;
    next.text_size = steps == 0 ? ui::kSmallestText : ui::step_text_size(g.look.text_size, steps);
    set_look(next, 0);
}

// ---- the window --------------------------------------------------------------------------------------------------

void on_timer() {
    WORD buttons = 0;
    g.connected = poll(buttons);
    if (g.capturing >= 0) {
        if (const std::uint16_t mask = newly_pressed(g.held, buttons); mask != 0u) {
            g.held = buttons;
            finish_capture(mask);
        } else if (static_cast<LONG>(GetTickCount() - g.capture_end) >= 0) {
            stop_capture();
        } else {
            g.held = static_cast<WORD>(g.held & buttons);  // a button let go can be pressed again
        }
    }
    if (g.key_capture >= 0 && static_cast<LONG>(GetTickCount() - g.key_capture_end) >= 0) stop_key_capture();
    if (g.page == kPadButtons) {
        for (std::size_t i = 0; i < kControls.size(); ++i) {
            const std::uint16_t mask = g.binding.*kControls[i].field;
            const bool down = g.capturing < 0 && mask != 0u && (buttons & mask) != 0u;
            set_text(item(kFirstLive + static_cast<int>(i)), down ? L"●" : L"");
        }
        refresh_picture(buttons);
    }
    if (static_cast<LONG>(GetTickCount() - g.next_game_check) >= 0) {
        g.game_running = game_is_running();
        g.next_game_check = GetTickCount() + 2000;
    }
    refresh_status();
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND: {
        RECT client;
        GetClientRect(hwnd, &client);
        FillRect(reinterpret_cast<HDC>(wparam), &client, ui::window_brush());
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(hwnd, &paint);
        for (const RECT& c : g.cards[static_cast<std::size_t>(g.page)]) {
            const RECT r{c.left, c.top - g.scroll_y, c.right, c.bottom - g.scroll_y};
            ui::paint_card(dc, r, px(6));
        }
        if (g.page == kPadButtons) {
            const RECT area = picture_area();
            const int w = area.right - area.left, h = area.bottom - area.top;
            // Off screen first, so a press does not flicker.
            HDC buffer = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max(w, 1), std::max(h, 1));
            HGDIOBJ old = SelectObject(buffer, bitmap);
            const RECT inside{0, 0, w, h};
            FillRect(buffer, &inside, ui::window_brush());
            paint_controller(buffer, inside, g.view, ui::colors().dark);
            BitBlt(dc, area.left, area.top, w, h, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, old);
            DeleteObject(bitmap);
            DeleteDC(buffer);
        }
        // A line above the bar at the bottom.
        RECT line{px(g.page_x + 12), px(g.bottom) - g.scroll_y, px(g.page_x + g.page_w - 12), px(g.bottom) + 1 - g.scroll_y};
        SetDCBrushColor(dc, ui::colors().surface_stroke);
        FillRect(dc, &line, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        const auto control = reinterpret_cast<HWND>(lparam);
        const int id = GetDlgCtrlID(control);
        if (control == g.tabs) {  // the navigation list is on the window, not a field
            SetBkColor(reinterpret_cast<HDC>(wparam), ui::colors().window);
            return reinterpret_cast<LRESULT>(ui::window_brush());
        }
        return ui::control_color(message, reinterpret_cast<HDC>(wparam), control,
                                 message == WM_CTLCOLORSTATIC && (id == kHelpId || id == kInfo), on_card(control));
    }
    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
        if (measure->CtlID == kTabs) measure->itemHeight = static_cast<UINT>(px(kNavItemH));
        return TRUE;
    }
    case WM_DRAWITEM: {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (draw->CtlID != kTabs) return DefWindowProcW(hwnd, message, wparam, lparam);
        if (draw->itemID == static_cast<UINT>(-1)) {
            FillRect(draw->hDC, &draw->rcItem, ui::window_brush());
            return TRUE;
        }
        const auto page = static_cast<std::size_t>(draw->itemID);
        if (page >= kPageCount) return TRUE;
        const bool focus = (draw->itemState & ODS_FOCUS) != 0 && (SendMessageW(g.tabs, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0;
        ui::paint_nav_item(draw->hDC, draw->rcItem, g.narrow_nav ? L"" : kPageNames[page].c_str(), kPageIcons[page],
                           (draw->itemState & ODS_SELECTED) != 0, focus, g.font, g.icons);
        return TRUE;
    }
    case WM_TIMER:
        if (wparam == kPollTimer) on_timer();
        return 0;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        LRESULT drawn = 0;
        if (ui::custom_draw(header, &drawn)) return drawn;
        return 0;
    }
    case WM_SIZE:
        // A new width lays the pages out again; so does a new height (the controller page fits the window's height).
        if (wparam != SIZE_MINIMIZED && !g.made.empty() && (layout_units() != g.layout_w || HIWORD(lparam) != g.layout_h)) relayout();
        else update_scrollbars();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        const SIZE smallest = smallest_window();
        info->ptMinTrackSize = {smallest.cx, smallest.cy};
        return 0;
    }
    case WM_VSCROLL:
        on_scrollbar(LOWORD(wparam));
        return 0;
    case WM_MOUSEWHEEL:
        scroll_to(g.scroll_y - GET_WHEEL_DELTA_WPARAM(wparam) * px(60) / WHEEL_DELTA);
        return 0;
    case kMsgLook:  // posted, so the list that was used is not remade while it is still telling about the choice
        set_look(ui::Appearance{static_cast<ui::Theme>(wparam), LOWORD(lparam)}, HIWORD(lparam));
        return 0;
    case WM_SETTINGCHANGE:
        // Windows' light or dark app mode (followed when the theme is "same as Windows"), its accent colour and its
        // high contrast mode: drawn again in the new colours.
        if (((lparam != 0 && lstrcmpW(reinterpret_cast<const wchar_t*>(lparam), L"ImmersiveColorSet") == 0) ||
             wparam == SPI_SETHIGHCONTRAST) &&
            ui::colors_differ(ui::is_dark(g.look.theme), g.look.theme == ui::Theme::Shadow))
            rebuild_view(0);
        return DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_LBUTTONDOWN: {
        if (g.page != kPadButtons) return 0;
        const int control = control_at(picture_area(), POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        if (control >= 0) start_capture(control);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT && g.page == kPadButtons) {
            POINT point;
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            if (control_at(picture_area(), point) >= 0) {
                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
                return TRUE;
            }
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_HSCROLL: {
        HWND from = reinterpret_cast<HWND>(lparam);
        if (from == nullptr) {  // the window has no sideways scroll bar
        } else if (from == item(kThreshold)) {
            g.binding.trigger_threshold = static_cast<std::uint8_t>(SendMessageW(item(kThreshold), TBM_GETPOS, 0, 0));
            set_text(item(kThresholdValue), std::to_wstring(g.binding.trigger_threshold));
            changed();
        } else if (Field* f = field_by_id(GetDlgCtrlID(from))) {
            on_field(*f, 0);
        }
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        const int code = HIWORD(wparam);
        if (id >= kFirstRow && id < kFirstRow + static_cast<int>(kControls.size())) {
            if (code == BN_CLICKED) start_capture(id - kFirstRow);
        } else if (id >= kFirstBind && id < kFirstBind + static_cast<int>(sp::kActionCount * sp::kMaxBinds)) {
            if (code == BN_CLICKED) start_key_capture(id - kFirstBind);
        } else if (id >= kFirstField && id < kFirstField + static_cast<int>(g.fields.size())) {
            on_field(g.fields[static_cast<std::size_t>(id - kFirstField)], code);
        } else if (id == kSwapSticks) {
            g.binding.swap_sticks = IsDlgButtonChecked(hwnd, kSwapSticks) == BST_CHECKED;
            changed();
        } else if (id == kStyle && code == CBN_SELCHANGE) {
            g.binding.button_style = kStyles[SendMessageW(item(kStyle), CB_GETCURSEL, 0, 0)].style;
            changed();
        } else if (id == kTabs && code == LBN_SELCHANGE) {
            const int page = static_cast<int>(SendMessageW(g.tabs, LB_GETCURSEL, 0, 0));
            if (page >= 0 && page < kPageCount && page != g.page) show_page(page);
        } else if (id == kTextSize && look_chosen(item(kTextSize), code)) {
            const auto i = static_cast<std::size_t>(SendMessageW(item(kTextSize), CB_GETCURSEL, 0, 0));
            if (i < std::size(kTextSizes)) PostMessageW(hwnd, kMsgLook, static_cast<WPARAM>(g.look.theme), MAKELPARAM(kTextSizes[i], kTextSize));
        } else if (id == kColours && look_chosen(item(kColours), code)) {
            const auto i = static_cast<std::size_t>(SendMessageW(item(kColours), CB_GETCURSEL, 0, 0));
            if (i < std::size(kThemes)) PostMessageW(hwnd, kMsgLook, static_cast<WPARAM>(kThemes[i].theme), MAKELPARAM(g.look.text_size, kColours));
        } else if (id == kRestore) {
            restore_defaults();
        } else if (id == kSave || id == IDOK) {
            save();
        } else if (id == kClose || id == IDCANCEL) {
            if (confirm_close()) DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_CLOSE:
        if (confirm_close()) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

std::filesystem::path this_program_folder() {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path(path).parent_path();
}

// Reads the four files. Problems are said once in the status line.
std::wstring load_all() {
    std::wstring notes;
    const auto note = [&](const std::wstring& text) {
        if (notes.empty()) notes = text;
    };
    std::string text, error;
    if (!read_file(g.folder / "display.cfg", text, nullptr, &error)) {
        note(wide(error));
    } else if (!read_display(text, g.display, &error)) {
        note("display.cfg has a line the game cannot read ("_w + wide(error) +
             "), so the game will not start with it. Defaults are shown; fix or delete the file before saving."_w);
    }
    g.display_saved = g.display;
    if (!read_file(g.folder / "options.cfg", text, nullptr, &error)) note(wide(error));
    else read_options(text, g.options);
    g.options_saved = g.options;
    if (!read_file(g.folder / "controls.cfg", text, nullptr, &error)) note(wide(error));
    else read_controls(text, g.keys);
    g.keys_saved = g.keys;
    if (load_binding(g.folder / "pad.cfg", g.binding, &error) == LoadResult::Invalid)
        note("pad.cfg has a problem, so the default buttons are shown: "_w + wide(error));
    g.saved = g.binding;
    g.view.binding = g.binding;
    return notes;
}

// --check-layout <report>: every page at every zoom, in the window's start-up size, its smallest size and a wide one,
// with the window shown but not activated. Each control whose text does not fit, each control off the window's side,
// any sideways scroll bar, and any zoom that changed the window's size is written to the report (UTF-8), one per line.
// Zoom is not stored. Returns the number of problems.
int check_layout(const std::filesystem::path& report_file) {
    g.checking = true;
    const ui::Appearance first = g.look;
    std::wstring report;
    int problems = 0;
    const auto problem = [&](const std::wstring& line) {
        report += line + L"\n";
        ++problems;
    };
    RECT start;
    GetWindowRect(g.window, &start);
    const SIZE smallest = smallest_window();
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(g.window, MONITOR_DEFAULTTONEAREST), &monitor);
    const struct {
        const wchar_t* name;
        int w, h;
    } sizes[] = {{L"start-up size", start.right - start.left, start.bottom - start.top},
                 {L"smallest size", smallest.cx, smallest.cy},
                 {L"wide", static_cast<int>(monitor.rcWork.right - monitor.rcWork.left),
                  std::min(static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top), MulDiv(800, g.dpi, 96))}};
    for (const auto& size : sizes) {
        SetWindowPos(g.window, nullptr, monitor.rcWork.left, monitor.rcWork.top, size.w, size.h, SWP_NOZORDER | SWP_NOACTIVATE);
        for (const int zoom : kTextSizes) {
            RECT before, after;
            GetWindowRect(g.window, &before);
            set_look(ui::Appearance{g.look.theme, zoom}, 0);
            GetWindowRect(g.window, &after);
            const std::wstring when = std::wstring(size.name) + L" " + std::to_wstring(before.right - before.left) + L"x" +
                                      std::to_wstring(before.bottom - before.top) + L", zoom " + std::to_wstring(zoom) + L" %";
            report += L"# " + when + L": page " + std::to_wstring(g.page_w) + L" units, " +
                      (g.narrow_nav ? L"icons-only navigation" : L"navigation with words") + L"\n";
            if (!EqualRect(&before, &after)) problem(when + L": the window's size changed");
            if ((GetWindowLongW(g.window, GWL_STYLE) & WS_HSCROLL) != 0) problem(when + L": a sideways scroll bar");
            for (int p = 0; p < kPageCount; ++p) {
                show_page(p);
                for (const std::wstring& line : ui::find_clipped(g.window, false)) problem(when + L", " + kPageNames[p] + L": " + line);
            }
            show_page(kDisplay);
        }
    }
    set_look(first, 0);
    SetWindowPos(g.window, nullptr, start.left, start.top, start.right - start.left, start.bottom - start.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    report += L"problems: " + std::to_wstring(problems) + L"\n";
    const int n = WideCharToMultiByte(CP_UTF8, 0, report.c_str(), static_cast<int>(report.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, report.c_str(), static_cast<int>(report.size()), utf8.data(), n, nullptr, nullptr);
    FILE* out = nullptr;
    if (_wfopen_s(&out, report_file.c_str(), L"wb") == 0 && out != nullptr) {
        fwrite(utf8.data(), 1, utf8.size(), out);
        fclose(out);
    }
    return problems;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&controls);
    Gdiplus::GdiplusStartupInput gdiplus_input;
    ULONG_PTR gdiplus_token = 0;
    Gdiplus::GdiplusStartup(&gdiplus_token, &gdiplus_input, nullptr);
    load_xinput();
    HDC screen = GetDC(nullptr);
    g.dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    g.folder = this_program_folder();
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i + 1 < argc; ++i)
            if (std::wstring(argv[i]) == L"--folder") g.folder = argv[i + 1];
        LocalFree(argv);
    }
    g.look = ui::load_appearance(g.folder);
    g.look.text_size = ui::step_text_size(g.look.text_size, 0);
    ui::set_dark(ui::is_dark(g.look.theme), g.look.theme == ui::Theme::Shadow);
    make_font();
    const std::wstring notes = load_all();
    g.game_running = game_is_running();
    g.next_game_check = GetTickCount() + 2000;

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = window_proc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    cls.lpszClassName = L"ShadowPadConfig";
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    cls.hIconSm = cls.hIcon;
    RegisterClassExW(&cls);

    // The window at the start: as wide as the pages at zoom 100 % and as tall as their content then, as far as the
    // screen allows. Zoom never changes its size; the player may resize it, and the pages follow.
    std::filesystem::path check_file;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i + 1 < argc; ++i)
            if (std::wstring(argv[i]) == L"--check-layout") check_file = argv[i + 1];
        LocalFree(argv);
    }
    RECT size{0, 0, MulDiv(kClientW, g.dpi, 96) + GetSystemMetrics(SM_CXVSCROLL), MulDiv(760, g.dpi, 96)};
    AdjustWindowRect(&size, kStyleFlags & ~WS_CLIPCHILDREN, FALSE);
    g.window = CreateWindowExW(0, cls.lpszClassName, kTitle.c_str(), kStyleFlags, CW_USEDEFAULT, CW_USEDEFAULT,
                               size.right - size.left, size.bottom - size.top, nullptr, nullptr, instance, nullptr);
    const int zoom = g.look.text_size;
    g.look.text_size = ui::kSmallestText;  // the content's height at zoom 100 % sets the window's
    rebuild_view(0);
    size_window_at_start(content_h());
    if (zoom != g.look.text_size) {
        g.look.text_size = zoom;
        rebuild_view(0);
    }
    show_page(kDisplay);
    if (!check_file.empty()) {
        const int problems = check_layout(check_file);
        DestroyWindow(g.window);
        Gdiplus::GdiplusShutdown(gdiplus_token);
        return problems > 0 ? 1 : 0;
    }
    if (!notes.empty()) show_message(notes);
    ShowWindow(g.window, show);
    SetTimer(g.window, kPollTimer, 16, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && g.capturing >= 0) {
            if (msg.wParam == VK_ESCAPE) {
                stop_capture();
                continue;
            }
            if (msg.wParam == VK_DELETE || msg.wParam == VK_BACK) {
                finish_capture(0u);
                continue;
            }
        }
        if (g.key_capture >= 0) {
            if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
                if (msg.wParam == VK_DELETE) finish_key_capture(sp::Bind{});
                else finish_key_capture(sp::key_bind(key_from_message(msg)));
                continue;
            }
            if (msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP || msg.message == WM_CHAR || msg.message == WM_SYSCHAR)
                continue;
            int button = -1;
            if (msg.message == WM_LBUTTONDOWN) button = sp::kMouseLeft;
            else if (msg.message == WM_RBUTTONDOWN) button = sp::kMouseRight;
            else if (msg.message == WM_MBUTTONDOWN) button = sp::kMouseMiddle;
            else if (msg.message == WM_XBUTTONDOWN) button = GET_XBUTTON_WPARAM(msg.wParam) == XBUTTON1 ? sp::kMouseX1 : sp::kMouseX2;
            if (button >= 0) {
                finish_key_capture(sp::mouse_bind(static_cast<sp::MouseButton>(button)));
                continue;
            }
            if (msg.message == WM_LBUTTONUP || msg.message == WM_RBUTTONUP || msg.message == WM_MBUTTONUP ||
                msg.message == WM_XBUTTONUP)
                continue;
        }
        // Ctrl and +, - or 0, and Ctrl with the mouse wheel: the text size, wherever the focus is.
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (msg.message == WM_KEYDOWN && ctrl && g.capturing < 0) {
            const WPARAM k = msg.wParam;
            if (k == VK_OEM_PLUS || k == VK_ADD || k == VK_OEM_MINUS || k == VK_SUBTRACT || k == '0' || k == VK_NUMPAD0) {
                step_text(k == VK_OEM_PLUS || k == VK_ADD ? 1 : k == '0' || k == VK_NUMPAD0 ? 0 : -1);
                continue;
            }
        }
        if (msg.message == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(msg.wParam) & MK_CONTROL) != 0) {
            step_text(GET_WHEEL_DELTA_WPARAM(msg.wParam) > 0 ? 1 : -1);
            continue;
        }
        if (IsDialogMessageW(g.window, &msg)) {
            show_focus();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        show_focus();
    }
    Gdiplus::GdiplusShutdown(gdiplus_token);
    return 0;
}
