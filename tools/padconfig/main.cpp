// ShadowPadConfig.exe: the game's settings program ("Shadow the Hedgehog Recompiled - Settings"). It edits the files the
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

const std::wstring kTitle = "Shadow the Hedgehog Recompiled - Settings"_w;

// Layout (96 dpi units at the normal text size; px() scales them by the screen and the chosen text size), in the
// manner of Windows 11's own Settings: a navigation list on the left (with the text size and colour choices under it),
// the chosen page's title, and each setting on a card. The pages are laid out in their own units, from x = 0; make()
// moves them right of the navigation list. The bar at the bottom is below the tallest page. At a large text size the
// window may not fit on the screen: it then scrolls.
constexpr int kNavW = 232;                    // the navigation column
constexpr int kPageX = kNavW - 8;             // where a page's x = 0 is
constexpr int kPageW = 1094;                  // a page's width (its controls run from 18 to 1082)
constexpr int kClientW = kPageX + kPageW;
constexpr int kTop = 72;                      // below the page title
constexpr int kPadBottom = kTop + 586;        // the controller page's pictures end here
constexpr int kNavItemH = 40, kLookW = kNavW - 28;

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
    int origin_x = 0;     // what make() adds to x: kPageX for the pages and the bottom bar, 0 for the navigation
    int bottom = kPadBottom;  // where the bar at the bottom starts (below the tallest page)
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
    int scroll_x = 0, scroll_y = 0;  // how far the window's content is scrolled
    HWND last_focus = nullptr;
} g;

std::wstring number_text(double v) { return wide(format_number(v)); }
int px(int v) { return MulDiv(v, g.dpi * g.look.text_size, 96 * 100); }
HWND item(int id) { return GetDlgItem(g.window, id); }

// ---- scrolling (only at a text size too large for the screen) ------------------------------------------------------

int content_w() { return px(kClientW); }
int content_h() { return px(g.bottom + 70); }
bool on_card(HWND h) { return std::find(g.on_card.begin(), g.on_card.end(), h) != g.on_card.end(); }

// Moves the content so that (x, y) of it is at the window's top left. Every control is moved by hand: the window may
// have controls outside its visible part, which ScrollWindowEx leaves where they are.
void scroll_to(int x, int y) {
    RECT client;
    GetClientRect(g.window, &client);
    x = std::clamp(x, 0, std::max(0, content_w() - static_cast<int>(client.right)));
    y = std::clamp(y, 0, std::max(0, content_h() - static_cast<int>(client.bottom)));
    const int dx = g.scroll_x - x, dy = g.scroll_y - y;
    if (dx == 0 && dy == 0) return;
    g.scroll_x = x;
    g.scroll_y = y;
    SetScrollPos(g.window, SB_HORZ, x, TRUE);
    SetScrollPos(g.window, SB_VERT, y, TRUE);
    int children = 0;
    for (HWND c = GetWindow(g.window, GW_CHILD); c != nullptr; c = GetWindow(c, GW_HWNDNEXT)) ++children;
    HDWP moves = BeginDeferWindowPos(children);
    for (HWND c = GetWindow(g.window, GW_CHILD); c != nullptr && moves != nullptr; c = GetWindow(c, GW_HWNDNEXT)) {
        RECT r;
        GetWindowRect(c, &r);
        MapWindowPoints(nullptr, g.window, reinterpret_cast<POINT*>(&r), 2);
        moves = DeferWindowPos(moves, c, nullptr, r.left + dx, r.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (moves != nullptr) EndDeferWindowPos(moves);
    InvalidateRect(g.window, nullptr, TRUE);
}

// The scroll bars for the window's present size: none when everything fits.
void update_scrollbars() {
    static bool busy = false;  // showing a bar resizes the window, which comes back here
    if (busy) return;
    busy = true;
    for (int pass = 0; pass < 2; ++pass) {  // a bar for one direction can make the other one needed
        RECT client;
        GetClientRect(g.window, &client);
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_RANGE | SIF_PAGE;
        info.nMax = content_w() - 1;
        info.nPage = static_cast<UINT>(client.right);
        SetScrollInfo(g.window, SB_HORZ, &info, TRUE);
        info.nMax = content_h() - 1;
        info.nPage = static_cast<UINT>(client.bottom);
        SetScrollInfo(g.window, SB_VERT, &info, TRUE);
    }
    scroll_to(g.scroll_x, g.scroll_y);
    busy = false;
}

// A scroll bar was used: `bar` is SB_HORZ or SB_VERT, `code` the WM_xSCROLL request.
void on_scrollbar(int bar, int code) {
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_ALL;
    GetScrollInfo(g.window, bar, &info);
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
    if (bar == SB_HORZ) scroll_to(pos, g.scroll_y);
    else scroll_to(g.scroll_x, pos);
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
    int x = g.scroll_x, y = g.scroll_y;
    if (r.left < 0) x += r.left - margin;
    else if (r.right > client.right) x += std::min(r.left - margin, r.right - client.right + margin);
    if (r.top < 0) y += r.top - margin;
    else if (r.bottom > client.bottom) y += std::min(r.top - margin, r.bottom - client.bottom + margin);
    scroll_to(x, y);
}

void set_text(HWND h, const std::wstring& text) {
    const int length = GetWindowTextLengthW(h);
    std::wstring now(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(h, now.data(), length + 1);
    now.resize(static_cast<std::size_t>(length));
    if (text != now) SetWindowTextW(h, text.c_str());
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
    set_text(item(kStatus), text);
    set_text(item(kInfo), g.game_running
                              ? "The game is running. You can still save: it uses the new settings the next time it starts."_w
                              : "The game reads these settings when it starts, so changes take effect the next time you start it."_w);
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
    return RECT{px(kPageX + 414) - g.scroll_x, px(kTop + 4) - g.scroll_y, px(kPageX + 1082) - g.scroll_x,
                px(kPadBottom - 4) - g.scroll_y};
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

HWND make(const wchar_t* cls, const std::wstring& text, DWORD style, int x, int y, int w, int h, int id, HFONT font = nullptr) {
    HWND c = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | (g.building < 0 ? WS_VISIBLE : 0) | style, px(g.origin_x + x),
                             px(y), px(w), px(h), g.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font != nullptr ? font : g.font), TRUE);
    if (g.building >= 0) g.page_controls[static_cast<std::size_t>(g.building)].push_back(c);
    if (g.card_open) g.on_card.push_back(c);
    return c;
}

// A card behind controls of the page being built, in page units.
void add_card(int left, int top, int right, int bottom) {
    g.cards[static_cast<std::size_t>(g.building)].push_back(RECT{g.origin_x + left, top, g.origin_x + right, bottom});
}

// How tall `text` is in `font` when wrapped to `width` units, in units.
int text_height(const std::wstring& text, HFONT font, int width) {
    if (text.empty()) return 0;
    HDC dc = GetDC(g.window);
    HGDIOBJ old = SelectObject(dc, font);
    RECT r{0, 0, px(width), 0};
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(g.window, dc);
    return MulDiv(r.bottom - r.top, 96 * 100, g.dpi * g.look.text_size) + 1;
}

constexpr int kHelpId = 900;  // the grey explanations (WM_CTLCOLORSTATIC)

void intro(const std::wstring& text) {
    make(L"STATIC", text, SS_LEFT | SS_NOPREFIX, 24, kTop, 1040, text_height(text, g.font, 1040) + 2, kHelpId);
}

// One setting on its own card, as in Windows' Settings: the name and its explanation on the left, the control on the
// right. Returns the card's height.
constexpr int kCardLeft = 18, kCardRight = 1082;
int place_card(Field& f, int y) {
    f.id = kFirstField + static_cast<int>(&f - g.fields.data());
    const int control_w = f.kind == Kind::Check ? 120 : f.kind == Kind::Number ? 110 : 320;
    const int control_x = kCardRight - 20 - control_w;
    const int text_x = kCardLeft + 20, text_w = control_x - 32 - text_x;
    const int label_h = text_height(f.label, g.font, text_w), help_h = text_height(f.help, g.caption, text_w);
    const int h = std::max(64, 14 + label_h + (help_h > 0 ? 3 + help_h : 0) + 14);
    g.card_open = true;
    add_card(kCardLeft, y, kCardRight, y + h);
    make(L"STATIC", f.label, SS_LEFT | SS_NOPREFIX, text_x, y + 14, text_w, label_h, -1);
    if (help_h > 0) make(L"STATIC", f.help, SS_LEFT | SS_NOPREFIX, text_x, y + 14 + label_h + 3, text_w, help_h, kHelpId, g.caption);
    const int cy = y + (h - 30) / 2;
    switch (f.kind) {
        case Kind::Check:
            f.control = make(L"BUTTON", "Off"_w, BS_AUTOCHECKBOX | WS_TABSTOP, control_x + 34, cy, control_w - 34, 30, f.id);
            ui::set_switch(f.control);
            break;
        case Kind::Combo:
            f.control = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, control_x, cy + 1, control_w, 300, f.id);
            break;
        case Kind::Slider:
            f.control = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, control_x, cy, control_w - 100, 30, f.id);
            SendMessageW(f.control, TBM_SETRANGE, TRUE, MAKELPARAM(static_cast<int>(f.low), static_cast<int>(f.high)));
            f.value = make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, control_x + control_w - 94, cy, 94, 30, -1);
            break;
        case Kind::Number:
            f.control = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, control_x, cy + 2, control_w, 26, f.id);
            break;
        case Kind::Size:
            f.control = make(L"COMBOBOX", L"", CBS_DROPDOWN | WS_TABSTOP | WS_VSCROLL, control_x, cy + 1, control_w, 300, f.id);
            for (const Choice& c : f.choices) SendMessageW(f.control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(c.label.c_str()));
            break;
    }
    g.card_open = false;
    return h;
}

// One row: the label, the control, and a line of explanation beside it.
void place(Field& f, int x, int y, int label_w, int control_w, int help_x, int help_w) {
    f.id = kFirstField + static_cast<int>(&f - g.fields.data());
    make(L"STATIC", f.label.c_str(), SS_LEFT | SS_NOPREFIX, x, y + 4, label_w, 40, -1);
    const int cx = x + label_w + 6;
    switch (f.kind) {
        case Kind::Check:
            f.control = make(L"BUTTON", "Off"_w, BS_AUTOCHECKBOX | WS_TABSTOP, cx, y, control_w, 26, f.id);
            ui::set_switch(f.control);
            break;
        case Kind::Combo:
            f.control = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, cx, y, control_w, 300, f.id);
            break;
        case Kind::Slider: {
            f.control = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, cx, y, control_w - 96, 26, f.id);
            SendMessageW(f.control, TBM_SETRANGE, TRUE, MAKELPARAM(static_cast<int>(f.low), static_cast<int>(f.high)));
            f.value = make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, cx + control_w - 90, y, 90, 26, -1);
            break;
        }
        case Kind::Number:
            f.control = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, cx, y, 90, 26, f.id);
            break;
        case Kind::Size: {
            f.control = make(L"COMBOBOX", L"", CBS_DROPDOWN | WS_TABSTOP | WS_VSCROLL, cx, y, control_w, 300, f.id);
            for (const Choice& c : f.choices)
                SendMessageW(f.control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(c.label.c_str()));
            break;
        }
    }
    if (!f.help.empty()) make(L"STATIC", f.help.c_str(), SS_LEFT | SS_NOPREFIX, help_x, y + 2, help_w, 56, kHelpId);
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

void build_pad_page() {
    constexpr int kRowH = 30, kLeft = 28;
    int y = kTop + 12;
    g.card_open = true;
    make(L"STATIC", "Game button"_w, SS_LEFT, kLeft, y, 120, 20, -1);
    make(L"STATIC", "Your controller (click to change)"_w, SS_LEFT, 150, y, 240, 20, -1);
    y += 26;
    for (int i = 0; i < static_cast<int>(kControls.size()); ++i) {
        make(L"STATIC", wide(kControls[static_cast<std::size_t>(i)].name).c_str(), SS_LEFT | SS_CENTERIMAGE, kLeft, y, 120, 26, -1);
        make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, 150, y, 200, 26, kFirstRow + i);
        make(L"STATIC", L"", SS_CENTER | SS_CENTERIMAGE, 356, y, 24, 26, kFirstLive + i);
        y += kRowH;
    }
    y += 8;
    make(L"STATIC", "Trigger press needed"_w, SS_LEFT | SS_CENTERIMAGE, kLeft, y, 140, 30, -1);
    HWND slider = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 170, y, 180, 30, kThreshold);
    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 255));
    make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, 356, y, 36, 30, kThresholdValue);
    y += kRowH + 4;
    make(L"STATIC", "Button prompts"_w, SS_LEFT | SS_CENTERIMAGE, kLeft, y, 120, 26, -1);
    HWND combo = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 150, y, 200, 200, kStyle);
    for (const auto& s : kStyles) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.label.c_str()));
    y += kRowH + 4;
    make(L"BUTTON", "Move with the right stick, camera with the left"_w, BS_AUTOCHECKBOX | WS_TABSTOP, kLeft, y, 360, 26, kSwapSticks);
    g.card_open = false;
    add_card(12, kTop, 400, y + 40);
}

void build_keyboard_page() {
    int y = kTop + 12;
    g.card_open = true;
    make(L"STATIC", "Action"_w, SS_LEFT, 26, y, 160, 20, -1);
    make(L"STATIC", "Click a box, then press a key or a mouse button"_w, SS_LEFT | SS_NOPREFIX, 196, y, 400, 20, -1);
    y += 26;
    for (std::size_t a = 0; a < sp::kActionCount; ++a) {
        make(L"STATIC", kActionLabels[a], SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, 26, y, 168, 22, -1);
        for (std::size_t s = 0; s < sp::kMaxBinds; ++s)
            make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, 196 + static_cast<int>(s) * 132, y, 128, 22,
                 kFirstBind + static_cast<int>(a * sp::kMaxBinds + s));
        y += 24;
    }
    add_card(12, kTop, 606, y + 12);
    y = kTop + 16;
    for (Field& f : g.fields) {
        if (f.page != kKeyboard) continue;
        place(f, 636, y, 200, 220, 0, 0);
        y += 46;
    }
    const std::wstring note =
        "Mouse speed 1 is the normal speed; 2 turns twice as fast. Walk speed is how fast you move while holding "_w
        "Walk (36 is the default). Keyboard and mouse changes also apply when you switch back to a running game."_w;
    const int note_h = text_height(note, g.caption, 420);
    make(L"STATIC", note, SS_LEFT | SS_NOPREFIX, 636, y + 6, 420, note_h, kHelpId, g.caption);
    g.card_open = false;
    add_card(618, kTop, kCardRight, y + 6 + note_h + 16);
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

void build() {
    // The navigation column: the pages, then the text size and colours.
    g.origin_x = 0;
    g.tabs = make(L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
                  12, 16, kNavW - 24, kPageCount * kNavItemH + 4, kTabs);
    for (int i = 0; i < kPageCount; ++i) SendMessageW(g.tabs, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kPageNames[i].c_str()));
    SendMessageW(g.tabs, LB_SETITEMHEIGHT, 0, px(kNavItemH));
    const int look_y = 16 + kPageCount * kNavItemH + 24;
    HWND sizes = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 16, look_y, kLookW, 300, kTextSize);
    for (const int size : kTextSizes) {
        const std::wstring label = "Zoom: "_w + std::to_wstring(size) + " %"_w;
        SendMessageW(sizes, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    HWND themes = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 16, look_y + 40, kLookW, 300, kColours);
    for (const auto& t : kThemes) SendMessageW(themes, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.label.c_str()));

    // The pages, right of it.
    g.origin_x = kPageX;
    make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 22, 14, 1040, 44, kPageTitle, g.title);
    for (auto& cards : g.cards) cards.clear();
    g.on_card.clear();
    define_fields();
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
    g.bottom = kPadBottom;
    for (int p = 0; p < kPageCount; ++p) {
        g.building = p;
        if (p == kPadButtons) {
            build_pad_page();
            continue;
        }
        if (p == kKeyboard) {
            build_keyboard_page();
            continue;
        }
        intro(intros[p]);
        int y = kTop + text_height(intros[p], g.font, 1040) + 14;
        for (Field& f : g.fields) {
            if (f.page != p) continue;
            y += place_card(f, y) + 4;
        }
        g.bottom = std::max(g.bottom, y + 12);
    }
    for (const auto& cards : g.cards)
        for (const RECT& r : cards) g.bottom = std::max(g.bottom, static_cast<int>(r.bottom) + 12);
    g.building = -1;

    // The bar at the bottom, on every page.
    const int b = g.bottom;
    make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 22, b + 10, 600, 22, kStatus);
    make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 22, b + 34, 600, 34, kInfo, g.caption);
    HWND restore = make(L"BUTTON", "Restore defaults"_w, BS_PUSHBUTTON | WS_TABSTOP, 652, b + 16, 176, 34, kRestore);
    HWND save_button = make(L"BUTTON", "Save"_w, BS_DEFPUSHBUTTON | WS_TABSTOP, 836, b + 16, 120, 34, kSave);
    make(L"BUTTON", "Close"_w, BS_PUSHBUTTON | WS_TABSTOP, 964, b + 16, 118, 34, kClose);
    ui::set_icon(restore, 0xE777 /*restore*/);
    ui::set_icon(save_button, 0xE74E /*save*/);
    ui::set_primary(save_button);
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
    InvalidateRect(g.window, nullptr, TRUE);  // the page's cards    refresh_rows();
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
}

void refresh_look() {
    for (int i = 0; i < static_cast<int>(std::size(kTextSizes)); ++i)
        if (kTextSizes[i] == g.look.text_size) SendMessageW(item(kTextSize), CB_SETCURSEL, static_cast<WPARAM>(i), 0);
    for (int i = 0; i < static_cast<int>(std::size(kThemes)); ++i)
        if (kThemes[i].theme == g.look.theme) SendMessageW(item(kColours), CB_SETCURSEL, static_cast<WPARAM>(i), 0);
}

// The window as large as its content, but no larger than the screen it is on (it scrolls then).
void fit_window() {
    constexpr DWORD kNoBars = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME;
    RECT want{0, 0, content_w(), content_h()};
    AdjustWindowRect(&want, kNoBars, FALSE);
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

// Makes every control again at the chosen text size and in the chosen colours. The settings being edited are kept;
// `focus` (a control id) gets the keyboard focus afterwards.
void rebuild_view(int focus) {
    const int page = g.page;
    if (focus == 0) {  // the control that has the focus now
        HWND f = GetFocus();
        while (f != nullptr && GetParent(f) != g.window) f = GetParent(f);
        if (f != nullptr) focus = GetDlgCtrlID(f);
    }
    SendMessageW(g.window, WM_SETREDRAW, FALSE, 0);
    for (HWND c = GetWindow(g.window, GW_CHILD); c != nullptr; c = GetWindow(g.window, GW_CHILD)) DestroyWindow(c);
    for (auto& controls : g.page_controls) controls.clear();
    g.on_card.clear();
    g.fields.clear();
    g.refused.clear();
    g.scroll_x = g.scroll_y = 0;
    g.last_focus = nullptr;
    ui::set_dark(ui::is_dark(g.look.theme), g.look.theme == ui::Theme::Shadow);
    make_font();
    build();
    refresh_fields();
    refresh_rows();
    refresh_pad_options();
    refresh_binds();
    refresh_look();
    ui::apply_to_window(g.window);
    show_page(page);
    SendMessageW(g.window, WM_SETREDRAW, TRUE, 0);
    fit_window();
    update_scrollbars();
    if (focus > 0 && item(focus) != nullptr) SetFocus(item(focus));
    RedrawWindow(g.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

// A new text size or colour choice: kept in appearance.cfg (the Mod Manager uses it too) and shown at once.
void set_look(ui::Appearance next, int focus) {
    next.text_size = ui::step_text_size(next.text_size, 0);
    if (next == g.look) return;
    const bool size_changed = next.text_size != g.look.text_size;
    g.look = next;
    const bool stored = ui::save_appearance(g.folder, g.look);
    rebuild_view(focus);
    if (!stored)
        show_message("The zoom and theme could not be stored in appearance.cfg, so they are used only until this "_w
                     "window closes."_w);
    else if (size_changed)
        show_message("Zoom "_w + std::to_wstring(g.look.text_size) +
                     " %. Ctrl and + or - also zoom in and out, and Ctrl and 0 goes back to 100 %."_w);
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
            const RECT r{px(c.left) - g.scroll_x, px(c.top) - g.scroll_y, px(c.right) - g.scroll_x, px(c.bottom) - g.scroll_y};
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
        RECT line{px(kPageX + 12) - g.scroll_x, px(g.bottom) - g.scroll_y, px(kClientW - 12) - g.scroll_x,
                  px(g.bottom) + 1 - g.scroll_y};
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
        ui::paint_nav_item(draw->hDC, draw->rcItem, kPageNames[page].c_str(), kPageIcons[page],
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
        update_scrollbars();
        return 0;
    case WM_GETMINMAXINFO: {
        // No larger than the content; small enough to fit any screen (it scrolls).
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        RECT most{0, 0, content_w(), content_h()};
        AdjustWindowRect(&most, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME, FALSE);
        info->ptMaxTrackSize = {most.right - most.left, most.bottom - most.top};
        info->ptMinTrackSize = {std::min(static_cast<LONG>(MulDiv(480, g.dpi, 96)), most.right - most.left),
                                std::min(static_cast<LONG>(MulDiv(360, g.dpi, 96)), most.bottom - most.top)};
        return 0;
    }
    case WM_VSCROLL:
        on_scrollbar(SB_VERT, LOWORD(wparam));
        return 0;
    case WM_MOUSEWHEEL:
        scroll_to(g.scroll_x, g.scroll_y - GET_WHEEL_DELTA_WPARAM(wparam) * px(60) / WHEEL_DELTA);
        return 0;
    case kMsgLook:  // posted, so the list that was used is not remade while it is still telling about the choice
        set_look(ui::Appearance{static_cast<ui::Theme>(wparam), LOWORD(lparam)}, HIWORD(lparam));
        return 0;
    case WM_SETTINGCHANGE:
        // Windows' light or dark app mode, followed when the colours are "as Windows".
        if (lparam != 0 && lstrcmpW(reinterpret_cast<const wchar_t*>(lparam), L"ImmersiveColorSet") == 0 &&
            g.look.theme == ui::Theme::System && ui::is_dark(ui::Theme::System) != ui::colors().dark)
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
        if (from == nullptr) {  // the window's own scroll bar
            on_scrollbar(SB_HORZ, LOWORD(wparam));
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
        } else if (id == kTextSize && code == CBN_SELCHANGE) {
            const auto i = static_cast<std::size_t>(SendMessageW(item(kTextSize), CB_GETCURSEL, 0, 0));
            if (i < std::size(kTextSizes)) PostMessageW(hwnd, kMsgLook, static_cast<WPARAM>(g.look.theme), MAKELPARAM(kTextSizes[i], kTextSize));
        } else if (id == kColours && code == CBN_SELCHANGE) {
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

    // Resizable only so that a large text size can be scrolled on a small screen; never larger than the content.
    constexpr DWORD kStyleFlags = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME | WS_CLIPCHILDREN;
    RECT size{0, 0, px(kClientW), px(kPadBottom + 70)};  // fit_window() sets the size once the pages are built
    AdjustWindowRect(&size, kStyleFlags, FALSE);
    g.window = CreateWindowExW(0, cls.lpszClassName, kTitle.c_str(), kStyleFlags, CW_USEDEFAULT, CW_USEDEFAULT,
                               size.right - size.left, size.bottom - size.top, nullptr, nullptr, instance, nullptr);
    build();
    refresh_fields();
    refresh_rows();
    refresh_pad_options();
    refresh_binds();
    refresh_look();
    ui::apply_to_window(g.window);
    show_page(kDisplay);
    fit_window();
    update_scrollbars();
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
