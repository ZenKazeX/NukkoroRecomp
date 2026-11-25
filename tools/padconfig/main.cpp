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

#include "controller_view.h"
#include "gekko/display_geometry.h"
#include "pad_config.h"
#include "settings_model.h"

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

// Layout (96 dpi units). The page area sits between the tab strip and the bar at the bottom.
constexpr int kClientW = 1094, kTop = 40, kPageH = 586, kBottom = kTop + kPageH, kClientH = kBottom + 70;

constexpr int kFirstRow = 100, kFirstLive = 200;
enum : int { kThreshold = 300, kThresholdValue, kSwapSticks, kStyle };
enum : int { kTabs = 400, kRestore, kSave, kClose, kStatus, kInfo };
constexpr int kFirstField = 1000, kFirstBind = 2000;
constexpr UINT_PTR kPollTimer = 1;
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
    HWND window = nullptr, tabs = nullptr;
    HFONT font = nullptr;
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
} g;

std::wstring number_text(double v) { return wide(format_number(v)); }
int px(int v) { return MulDiv(v, g.dpi, 96); }
HWND item(int id) { return GetDlgItem(g.window, id); }

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

RECT picture_area() { return RECT{px(414), px(kTop + 4), px(1082), px(kBottom - 4)}; }

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
    HWND c = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | (g.building < 0 ? WS_VISIBLE : 0) | style, px(x), px(y), px(w), px(h),
                             g.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font != nullptr ? font : g.font), TRUE);
    if (g.building >= 0) g.page_controls[static_cast<std::size_t>(g.building)].push_back(c);
    return c;
}

constexpr int kHelpId = 900;  // the grey explanations (WM_CTLCOLORSTATIC)

void intro(const std::wstring& text) { make(L"STATIC", text, SS_LEFT | SS_NOPREFIX, 24, kTop + 12, 1040, 40, kHelpId); }

// One row: the label, the control, and a line of explanation beside it.
void place(Field& f, int x, int y, int label_w, int control_w, int help_x, int help_w) {
    f.id = kFirstField + static_cast<int>(&f - g.fields.data());
    make(L"STATIC", f.label.c_str(), SS_LEFT | SS_NOPREFIX, x, y + 4, label_w, 40, -1);
    const int cx = x + label_w + 6;
    switch (f.kind) {
        case Kind::Check:
            f.control = make(L"BUTTON", "On"_w, BS_AUTOCHECKBOX | WS_TABSTOP, cx, y, control_w, 26, f.id);
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
    constexpr int kRowH = 30, kLeft = 18;
    int y = kTop + 8;
    make(L"STATIC", "Game button"_w, SS_LEFT, kLeft, y, 130, 20, -1);
    make(L"STATIC", "Your controller (click to change)"_w, SS_LEFT, 150, y, 260, 20, -1);
    y += 24;
    for (int i = 0; i < static_cast<int>(kControls.size()); ++i) {
        make(L"STATIC", wide(kControls[static_cast<std::size_t>(i)].name).c_str(), SS_LEFT | SS_CENTERIMAGE, kLeft, y, 120, 26, -1);
        make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, 150, y, 200, 26, kFirstRow + i);
        make(L"STATIC", L"", SS_CENTER | SS_CENTERIMAGE, 356, y, 24, 26, kFirstLive + i);
        y += kRowH;
    }
    y += 8;
    make(L"STATIC", "Trigger press needed"_w, SS_LEFT | SS_CENTERIMAGE, kLeft, y, 130, 26, -1);
    HWND slider = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 150, y, 200, 26, kThreshold);
    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 255));
    make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, 356, y, 40, 26, kThresholdValue);
    y += kRowH;
    make(L"STATIC", "Button prompts"_w, SS_LEFT | SS_CENTERIMAGE, kLeft, y, 130, 26, -1);
    HWND combo = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 150, y, 200, 200, kStyle);
    for (const auto& s : kStyles) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.label.c_str()));
    y += kRowH;
    make(L"BUTTON", "Move with the right stick, camera with the left"_w, BS_AUTOCHECKBOX | WS_TABSTOP, kLeft, y, 370, 24, kSwapSticks);
}

void build_keyboard_page() {
    int y = kTop + 10;
    make(L"STATIC", "Action"_w, SS_LEFT, 18, y, 170, 20, -1);
    make(L"STATIC", "Click a box, then press a key or a mouse button"_w, SS_LEFT | SS_NOPREFIX, 196, y, 400, 20, -1);
    y += 24;
    for (std::size_t a = 0; a < sp::kActionCount; ++a) {
        make(L"STATIC", kActionLabels[a], SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, 18, y, 176, 22, -1);
        for (std::size_t s = 0; s < sp::kMaxBinds; ++s)
            make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, 196 + static_cast<int>(s) * 134, y, 130, 22,
                 kFirstBind + static_cast<int>(a * sp::kMaxBinds + s));
        y += 24;
    }
    y = kTop + 34;
    for (Field& f : g.fields) {
        if (f.page != kKeyboard) continue;
        place(f, 620, y, 220, 220, 0, 0);
        y += 44;
    }
    make(L"STATIC",
         "Mouse speed 1 is the normal speed; 2 turns twice as fast. Walk speed is how fast you move while holding "_w
         "Walk (36 is the default). Keyboard and mouse changes also apply when you switch back to a running game."_w,
         SS_LEFT | SS_NOPREFIX, 620, y + 6, 450, 110, kHelpId);
}

void build() {
    g.tabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, px(8), px(6),
                             px(kClientW - 16), px(30), g.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTabs)),
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(g.tabs, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    for (int i = 0; i < kPageCount; ++i) {
        TCITEMW tab{};
        tab.mask = TCIF_TEXT;
        tab.pszText = const_cast<wchar_t*>(kPageNames[i].c_str());
        SendMessageW(g.tabs, TCM_INSERTITEMW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&tab));
    }

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
        int y = kTop + 50;
        for (Field& f : g.fields) {
            if (f.page != p) continue;
            place(f, 24, y, 230, 300, 570, 500);
            y += 62;
        }
    }
    g.building = -1;

    // The bar at the bottom, on every page.
    make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 18, kBottom + 8, 640, 22, kStatus);
    make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 18, kBottom + 32, 640, 34, kInfo);
    make(L"BUTTON", "Restore defaults"_w, BS_PUSHBUTTON | WS_TABSTOP, 680, kBottom + 16, 160, 32, kRestore);
    make(L"BUTTON", "Save"_w, BS_DEFPUSHBUTTON | WS_TABSTOP, 850, kBottom + 16, 110, 32, kSave);
    make(L"BUTTON", "Close"_w, BS_PUSHBUTTON | WS_TABSTOP, 968, kBottom + 16, 110, 32, kClose);
}

void show_page(int page) {
    g.capturing = -1;
    g.key_capture = -1;
    g.page = page;
    for (int p = 0; p < kPageCount; ++p)
        for (HWND h : g.page_controls[static_cast<std::size_t>(p)]) ShowWindow(h, p == page ? SW_SHOW : SW_HIDE);
    SendMessageW(g.tabs, TCM_SETCURSEL, static_cast<WPARAM>(page), 0);
    const RECT area = picture_area();
    InvalidateRect(g.window, &area, TRUE);
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
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(hwnd, &paint);
        if (g.page == kPadButtons) {
            const RECT area = picture_area();
            // Off screen first, so a press does not flicker.
            HDC buffer = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, area.right, area.bottom);
            HGDIOBJ old = SelectObject(buffer, bitmap);
            FillRect(buffer, &area, GetSysColorBrush(COLOR_BTNFACE));
            paint_controller(buffer, area, g.view);
            BitBlt(dc, area.left, area.top, area.right - area.left, area.bottom - area.top, buffer, area.left, area.top, SRCCOPY);
            SelectObject(buffer, old);
            DeleteObject(bitmap);
            DeleteDC(buffer);
        }
        // A line above the bar at the bottom.
        RECT line{px(8), px(kBottom), px(kClientW - 8), px(kBottom) + 1};
        FillRect(dc, &line, GetSysColorBrush(COLOR_3DSHADOW));
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lparam));
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        if (id == kHelpId || id == kInfo) SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_TIMER:
        if (wparam == kPollTimer) on_timer();
        return 0;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header->idFrom == kTabs && header->code == TCN_SELCHANGE)
            show_page(static_cast<int>(SendMessageW(g.tabs, TCM_GETCURSEL, 0, 0)));
        return 0;
    }
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
        if (from == item(kThreshold)) {
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
    g.font = CreateFontW(-MulDiv(10, g.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    g.folder = this_program_folder();
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i + 1 < argc; ++i)
            if (std::wstring(argv[i]) == L"--folder") g.folder = argv[i + 1];
        LocalFree(argv);
    }
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

    constexpr DWORD kStyleFlags = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT size{0, 0, px(kClientW), px(kClientH)};
    AdjustWindowRect(&size, kStyleFlags, FALSE);
    g.window = CreateWindowExW(0, cls.lpszClassName, kTitle.c_str(), kStyleFlags, CW_USEDEFAULT, CW_USEDEFAULT,
                               size.right - size.left, size.bottom - size.top, nullptr, nullptr, instance, nullptr);
    build();
    refresh_fields();
    refresh_rows();
    refresh_pad_options();
    refresh_binds();
    show_page(kDisplay);
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
        if (IsDialogMessageW(g.window, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Gdiplus::GdiplusShutdown(gdiplus_token);
    return 0;
}
