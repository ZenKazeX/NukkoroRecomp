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

constexpr wchar_t kTitle[] = L"Shadow the Hedgehog Recompiled - Settings";

// Layout (96 dpi units). The page area sits between the tab strip and the bar at the bottom.
constexpr int kClientW = 1094, kTop = 40, kPageH = 586, kBottom = kTop + kPageH, kClientH = kBottom + 70;

constexpr int kFirstRow = 100, kFirstLive = 200;
enum : int { kThreshold = 300, kThresholdValue, kSwapSticks, kStyle };
enum : int { kTabs = 400, kRestore, kSave, kClose, kStatus, kInfo };
constexpr int kFirstField = 1000, kFirstBind = 2000;
constexpr UINT_PTR kPollTimer = 1;
constexpr DWORD kCaptureMs = 6000;

enum Page : int { kDisplay, kFrameRate, kSound, kLanguage, kPadButtons, kPadOptions, kKeyboard, kSaving, kPageCount };
const wchar_t* const kPageNames[kPageCount] = {L"Display",          L"Frame rate",         L"Sound",
                                               L"Language",         L"Controller buttons", L"Controller options",
                                               L"Keyboard and mouse", L"Saving"};

const struct {
    const wchar_t* label;
    sp::ButtonStyle style;
} kStyles[] = {{L"Automatic", sp::ButtonStyle::Auto},
               {L"Original (GameCube)", sp::ButtonStyle::Original},
               {L"Xbox", sp::ButtonStyle::Xbox},
               {L"PlayStation", sp::ButtonStyle::PlayStation},
               {L"Nintendo Switch", sp::ButtonStyle::Switch}};

// What each keyboard action is called on the page, in sp::Action order.
const wchar_t* const kActionLabels[sp::kActionCount] = {
    L"Move forward",      L"Move back",          L"Move left",        L"Move right",      L"Walk (hold)",
    L"Camera left",       L"Camera right",       L"Camera up",        L"Camera down",     L"Jump (A)",
    L"Attack (B)",        L"Action (X)",         L"Chaos power (Y)",  L"L button",        L"Move sideways (R, hold)",
    L"Z button",          L"Pause (Start)",      L"Back / pause",     L"Menu up (D-pad)", L"Menu down (D-pad)",
    L"Menu left (D-pad)", L"Menu right (D-pad)"};

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

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
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
        text = L"Press the controller button for " + wide(kControls[static_cast<std::size_t>(g.capturing)].name) +
               L". Esc cancels, Delete clears it.";
    } else if (g.key_capture >= 0) {
        text = L"Press a key or a mouse button for " +
               std::wstring(kActionLabels[static_cast<std::size_t>(g.key_capture) / sp::kMaxBinds]) +
               L". Delete clears this box. Wait a few seconds to cancel.";
    } else if (g.status_is_message) {
        text = g.message;
    } else if (!g.refused.empty()) {
        text = g.refused.begin()->second;
    } else if (g.page == kPadButtons && !g.connected) {
        text = unsaved() ? L"Unsaved changes. No controller found: connect one to choose buttons."
                         : L"No controller found. Connect one to choose buttons.";
    } else if (unsaved()) {
        text = L"You have unsaved changes. Click Save to keep them.";
    } else if (g.page == kPadButtons) {
        text = L"Controller connected. Click a button on the picture or in the list to change it.";
    } else {
        text = L"No unsaved changes.";
    }
    set_text(item(kStatus), text);
    set_text(item(kInfo), g.game_running
                              ? L"The game is running. You can still save: it uses the new settings the next time it starts."
                              : L"The game reads these settings when it starts, so changes take effect the next time you start it.");
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
    bool found = false;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !found; ok = Process32NextW(snapshot, &entry))
        found = lstrcmpiW(entry.szExeFile, L"ShadowTheHedgehog.exe") == 0;
    CloseHandle(snapshot);
    return found;
}

// ---- the controller buttons page (unchanged behaviour) -----------------------------------------------------------

std::wstring row_text(std::size_t i) { return control_label(g.binding, i); }

void refresh_rows() {
    for (std::size_t i = 0; i < kControls.size(); ++i) {
        const bool waiting = g.capturing == static_cast<int>(i);
        set_text(item(kFirstRow + static_cast<int>(i)), waiting ? L"Press a button..." : row_text(i));
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
        static const wchar_t* const names[] = {L"Left mouse button", L"Right mouse button", L"Middle mouse button",
                                               L"Mouse back button", L"Mouse forward button"};
        return bind.code < std::size(names) ? names[bind.code] : L"Mouse button";
    }
    if (bind.source != sp::Bind::Source::Key) return L"-";
    const std::string name = sp::key_name(bind.code);
    static const struct {
        const char* from;
        const wchar_t* to;
    } friendly[] = {{"LShift", L"Left Shift"}, {"RShift", L"Right Shift"}, {"LCtrl", L"Left Ctrl"},
                    {"RCtrl", L"Right Ctrl"},  {"LAlt", L"Left Alt"},      {"RAlt", L"Right Alt"},
                    {"Up", L"Up arrow"},       {"Down", L"Down arrow"},    {"Left", L"Left arrow"},
                    {"Right", L"Right arrow"}};
    for (const auto& f : friendly)
        if (name == f.from) return f.to;
    return wide(name);
}

void refresh_binds() {
    for (std::size_t a = 0; a < sp::kActionCount; ++a)
        for (std::size_t s = 0; s < sp::kMaxBinds; ++s) {
            const int index = static_cast<int>(a * sp::kMaxBinds + s);
            set_text(item(kFirstBind + index),
                     g.key_capture == index ? L"Press a key..." : bind_text(g.keys.binds[a][s]));
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
            if (!names.empty()) names += L", ";
            names += kActionLabels[static_cast<std::size_t>(other)];
        }
        show_message(bind_text(bind) + L" is also used for: " + names + L". Both will happen when you press it.");
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
    return std::to_wstring(v / 100000) + L" x " + std::to_wstring(v % 100000);
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
                const std::wstring extra = (f.show ? f.show(v) : number_text(v)) + L" (set in the file)";
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

HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT font = nullptr) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | (g.building < 0 ? WS_VISIBLE : 0) | style, px(x), px(y), px(w), px(h),
                             g.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font != nullptr ? font : g.font), TRUE);
    if (g.building >= 0) g.page_controls[static_cast<std::size_t>(g.building)].push_back(c);
    return c;
}

constexpr int kHelpId = 900;  // the grey explanations (WM_CTLCOLORSTATIC)

void intro(const wchar_t* text) { make(L"STATIC", text, SS_LEFT | SS_NOPREFIX, 24, kTop + 12, 1040, 40, kHelpId); }

// One row: the label, the control, and a line of explanation beside it.
void place(Field& f, int x, int y, int label_w, int control_w, int help_x, int help_w) {
    f.id = kFirstField + static_cast<int>(&f - g.fields.data());
    make(L"STATIC", f.label.c_str(), SS_LEFT | SS_NOPREFIX, x, y + 4, label_w, 40, -1);
    const int cx = x + label_w + 6;
    switch (f.kind) {
        case Kind::Check:
            f.control = make(L"BUTTON", L"On", BS_AUTOCHECKBOX | WS_TABSTOP, cx, y, control_w, 26, f.id);
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

std::wstring percent(double v) { return number_text(v) + L" %"; }

// The game's own options are -1 until the game or the player sets them.
const Choice kNotChosen{L"Not chosen yet (the game decides)", -1.0};

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
    std::vector<Choice> out{{L"The same as the desktop", 0.0}};
    for (const auto& [w, h] : modes) out.push_back({size_text(size_value(w, h)), size_value(w, h)});
    return out;
}

void define_fields() {
    DisplayPrefs& d = g.display;
    OptionPrefs& o = g.options;
    ControlsConfig& k = g.keys;

    // Display
    add(kDisplay, Kind::Combo, L"Screen mode",
        L"Full screen covers the whole screen at your desktop's resolution. Exclusive full screen switches the monitor "
        L"to the resolution below. In the game, Alt+Enter switches between window and full screen.",
        [&] { return d.fullscreen ? (d.exclusive ? 2.0 : 1.0) : 0.0; },
        [&](double v) {
            d.fullscreen = v != 0.0;
            if (v != 0.0) d.exclusive = v == 2.0;
        },
        {{L"Window", 0.0}, {L"Full screen", 1.0}, {L"Exclusive full screen", 2.0}});
    add(kDisplay, Kind::Size, L"Window size",
        L"The size of the game's window, as width x height. Pick one or type your own, such as 1600 x 900. You can "
        L"also drag the window's edge in the game.",
        [&] { return size_value(d.width, d.height); },
        [&](double v) {
            const auto e = static_cast<long long>(v);
            d.width = static_cast<int>(e / 100000);
            d.height = static_cast<int>(e % 100000);
        },
        {{L"1280 x 720", size_value(1280, 720)},   {L"1366 x 768", size_value(1366, 768)},
         {L"1600 x 900", size_value(1600, 900)},   {L"1920 x 1080", size_value(1920, 1080)},
         {L"2560 x 1440", size_value(2560, 1440)}, {L"3840 x 2160", size_value(3840, 2160)},
         {L"1024 x 768", size_value(1024, 768)},   {L"1440 x 1080", size_value(1440, 1080)}});
    g.fields.back().bad = L"Window size: type it as width x height, for example 1600 x 900 (at least 320 x 240).";
    add(kDisplay, Kind::Combo, L"Full screen resolution",
        L"Used only by exclusive full screen. If your monitor cannot show it, close the game and choose another here.",
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
        std::vector<Choice> shapes{{L"Automatic (fills the window or screen)", 0.0}};
        for (const gekko::NamedAspect& a : gekko::kNamedAspects) {
            std::wstring label = wide(a.name);
            if (a.w == 4 && a.h == 3) label += L" (the original game)";
            shapes.push_back({label, static_cast<double>(a.w) / a.h});
        }
        add(kDisplay, Kind::Combo, L"Picture shape",
            L"Automatic uses the whole window or screen. A fixed shape adds black bars where it does not fit.",
            [&] { return d.aspect; }, [&](double v) { d.aspect = v; }, shapes);
        g.fields.back().show = [](double v) { return wide(gekko::aspect_label(v)); };
    }
    add(kDisplay, Kind::Slider, L"Resolution scale",
        L"How sharp the 3D picture is drawn. Below 100 % is faster but blurrier; above 100 % is sharper but needs a "
        L"stronger graphics card.",
        [&] { return d.render_scale; }, [&](double v) { d.render_scale = v; });
    g.fields.back().low = 25;
    g.fields.back().high = 200;
    g.fields.back().show = percent;
    add(kDisplay, Kind::Combo, L"Edge smoothing (anti-aliasing)",
        L"Smooths jagged edges of 3D objects. Higher is smoother but slower. If your graphics card cannot do the "
        L"chosen level, the game uses the nearest one it can.",
        [&] { return static_cast<double>(d.msaa); }, [&](double v) { d.msaa = static_cast<int>(v); },
        {{L"Off", 1}, {L"2x", 2}, {L"4x (default)", 4}, {L"8x", 8}, {L"16x", 16}, {L"32x", 32}});
    add(kDisplay, Kind::Combo, L"Texture sharpness (anisotropic filtering)",
        L"Keeps ground and wall textures sharp when seen at an angle. Off looks like the original console.",
        [&] { return static_cast<double>(d.anisotropy); }, [&](double v) { d.anisotropy = static_cast<int>(v); },
        {{L"Off", 1}, {L"2x", 2}, {L"4x", 4}, {L"8x", 8}, {L"16x (default)", 16}});
    add(kDisplay, Kind::Slider, L"Draw distance",
        L"How far away objects are still shown. 1 is the original game; higher shows more but needs more power.",
        [&] { return d.draw_distance; }, [&](double v) { d.draw_distance = v; });
    g.fields.back().low = 1;
    g.fields.back().high = 8;
    g.fields.back().show = [](double v) { return number_text(v) + (v == 1.0 ? L" (original)" : L""); };

    // Frame rate
    add(kFrameRate, Kind::Check, L"V-Sync",
        L"Waits for your monitor before showing each picture, so the picture never tears. Turn it off only for the "
        L"lowest delay or if your monitor has G-Sync or FreeSync.",
        [&] { return d.vsync ? 1.0 : 0.0; }, [&](double v) { d.vsync = v != 0.0; });
    add(kFrameRate, Kind::Combo, L"Frame rate",
        L"How many pictures per second are shown. The game itself always runs at its original speed: a higher rate "
        L"only looks smoother when Smooth motion is on.",
        [&] { return d.render_rate; }, [&](double v) { d.render_rate = v; },
        {{L"30 per second", 30}, {L"60 per second (default)", 60}, {L"75 per second", 75}, {L"90 per second", 90},
         {L"120 per second", 120}, {L"144 per second", 144}, {L"165 per second", 165}, {L"240 per second", 240},
         {L"360 per second", 360}, {L"Unlimited", 0}});
    g.fields.back().show = [](double v) { return v <= 0.0 ? std::wstring(L"Unlimited") : number_text(v) + L" per second"; };
    add(kFrameRate, Kind::Check, L"Smooth motion (frame interpolation)",
        L"Blends the game's own frames to fill the extra pictures when the frame rate is above 60, for smoother "
        L"motion. It has no effect at 60 per second or less.",
        [&] { return d.frame_interpolation ? 1.0 : 0.0; }, [&](double v) { d.frame_interpolation = v != 0.0; });
    add(kFrameRate, Kind::Check, L"Frame counter",
        L"Shows the frames per second in the top right corner. In the game, the * key on the number pad switches it.",
        [&] { return d.fps_counter ? 1.0 : 0.0; }, [&](double v) { d.fps_counter = v != 0.0; });

    // Sound
    const auto volume = [&](const wchar_t* label, const wchar_t* help, int& value) {
        add(kSound, Kind::Slider, label, help, [&value] { return static_cast<double>(value); },
            [&value](double v) { value = static_cast<int>(v); });
        g.fields.back().low = 0;
        g.fields.back().high = 100;
        g.fields.back().show = percent;
    };
    volume(L"Master volume", L"The volume of everything the game plays.", o.master_volume);
    volume(L"Music", L"The music, below the master volume.", o.music_volume);
    volume(L"Sound effects", L"Jumps, rings, explosions and other effects, below the master volume.", o.sfx_volume);
    volume(L"Voices", L"Spoken lines in cutscenes and during play, below the master volume.", o.voice_volume);
    add(kSound, Kind::Combo, L"Speaker output",
        L"The same as the game's own Options > System. Choose Dolby Pro Logic II only for a surround receiver that "
        L"decodes it.",
        [&] { return static_cast<double>(o.speaker); }, [&](double v) { o.speaker = static_cast<int>(v); },
        {kNotChosen, {L"Stereo", 0}, {L"Mono", 1}, {L"Dolby Pro Logic II", 2}});

    // Language
    add(kLanguage, Kind::Combo, L"Spoken language",
        L"The language of the voices. Japanese needs the Japanese disc's voice files in a voice_jp folder of the game; "
        L"without them the voices stay English.",
        [&] { return static_cast<double>(o.voice); }, [&](double v) { o.voice = static_cast<int>(v); },
        {kNotChosen, {L"English", 1}, {L"Japanese", 0}});
    add(kLanguage, Kind::Check, L"Subtitles",
        L"Shows the text of spoken lines. Off hides only the text; the voices still play.",
        [&] { return o.subtitles ? 1.0 : 0.0; }, [&](double v) { o.subtitles = v != 0.0; });
    add(kLanguage, Kind::Combo, L"Text language",
        L"The language of menus and subtitles. The same as the game's own Options > Subtitle.",
        [&] { return static_cast<double>(o.language); }, [&](double v) { o.language = static_cast<int>(v); },
        {kNotChosen, {L"English", 1}, {L"French", 2}, {L"German", 4}, {L"Italian", 5}, {L"Spanish", 3}, {L"Japanese", 0}});

    // Controller options
    add(kPadOptions, Kind::Combo, L"Vibration",
        L"Whether the controller rumbles. The same as the game's own Options > Vibration.",
        [&] { return static_cast<double>(o.vibration); }, [&](double v) { o.vibration = static_cast<int>(v); },
        {kNotChosen, {L"On", 1}, {L"Off", 0}});
    add(kPadOptions, Kind::Slider, L"Rumble strength",
        L"How hard the controller rumbles when vibration is on. 100 % is the original strength.",
        [&] { return static_cast<double>(o.rumble_strength); }, [&](double v) { o.rumble_strength = static_cast<int>(v); });
    g.fields.back().low = 25;
    g.fields.back().high = 100;
    g.fields.back().show = percent;
    add(kPadOptions, Kind::Check, L"Invert camera left and right",
        L"Pushing the camera stick right turns the camera the other way.",
        [&] { return o.invert_camera_x ? 1.0 : 0.0; }, [&](double v) { o.invert_camera_x = v != 0.0; });
    add(kPadOptions, Kind::Check, L"Invert camera up and down",
        L"Pushing the camera stick up moves the camera the other way.",
        [&] { return o.invert_camera_y ? 1.0 : 0.0; }, [&](double v) { o.invert_camera_y = v != 0.0; });

    // Keyboard and mouse (the right-hand column of the page)
    const auto speed = [&](const wchar_t* label, double& value) {
        add(kKeyboard, Kind::Number, label, L"", [&value] { return value; }, [&value](double v) { value = v; });
        g.fields.back().low = 0.01;
        g.fields.back().high = 50;
        g.fields.back().bad = L"Mouse speed must be a number from 0.01 to 50, for example 1.5.";
    };
    speed(L"Mouse speed, left and right", k.mouse_sensitivity_x);
    speed(L"Mouse speed, up and down", k.mouse_sensitivity_y);
    add(kKeyboard, Kind::Check, L"Invert mouse left and right", L"", [&] { return k.mouse_invert_x ? 1.0 : 0.0; },
        [&](double v) { k.mouse_invert_x = v != 0.0; });
    add(kKeyboard, Kind::Check, L"Invert mouse up and down", L"", [&] { return k.mouse_invert_y ? 1.0 : 0.0; },
        [&](double v) { k.mouse_invert_y = v != 0.0; });
    add(kKeyboard, Kind::Check, L"Keep the pointer in the game", L"", [&] { return k.mouse_capture ? 1.0 : 0.0; },
        [&](double v) { k.mouse_capture = v != 0.0; });
    add(kKeyboard, Kind::Slider, L"Walk speed", L"", [&] { return static_cast<double>(k.walk_deflection); },
        [&](double v) { k.walk_deflection = static_cast<int>(v); });
    g.fields.back().low = 1;
    g.fields.back().high = 100;
    g.fields.back().show = [](double v) {
        return number_text(v) + (v == ControlsConfig::kDefaultWalkDeflection ? L" (default)" : L"");
    };

    // Saving
    add(kSaving, Kind::Combo, L"Autosave",
        L"On: the game saves by itself after stages and when you leave its Options screen. Off: it saves only when "
        L"you choose to.",
        [&] { return static_cast<double>(o.autosave); }, [&](double v) { o.autosave = static_cast<int>(v); },
        {kNotChosen, {L"On", 1}, {L"Off", 0}});
}

void build_pad_page() {
    constexpr int kRowH = 30, kLeft = 18;
    int y = kTop + 8;
    make(L"STATIC", L"Game button", SS_LEFT, kLeft, y, 130, 20, -1);
    make(L"STATIC", L"Your controller (click to change)", SS_LEFT, 150, y, 260, 20, -1);
    y += 24;
    for (int i = 0; i < static_cast<int>(kControls.size()); ++i) {
        make(L"STATIC", wide(kControls[static_cast<std::size_t>(i)].name).c_str(), SS_LEFT | SS_CENTERIMAGE, kLeft, y, 120, 26, -1);
        make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, 150, y, 200, 26, kFirstRow + i);
        make(L"STATIC", L"", SS_CENTER | SS_CENTERIMAGE, 356, y, 24, 26, kFirstLive + i);
        y += kRowH;
    }
    y += 8;
    make(L"STATIC", L"Trigger press needed", SS_LEFT | SS_CENTERIMAGE, kLeft, y, 130, 26, -1);
    HWND slider = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 150, y, 200, 26, kThreshold);
    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 255));
    make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, 356, y, 40, 26, kThresholdValue);
    y += kRowH;
    make(L"STATIC", L"Button prompts", SS_LEFT | SS_CENTERIMAGE, kLeft, y, 130, 26, -1);
    HWND combo = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 150, y, 200, 200, kStyle);
    for (const auto& s : kStyles) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.label));
    y += kRowH;
    make(L"BUTTON", L"Move with the right stick, camera with the left", BS_AUTOCHECKBOX | WS_TABSTOP, kLeft, y, 370, 24, kSwapSticks);
}

void build_keyboard_page() {
    int y = kTop + 10;
    make(L"STATIC", L"Action", SS_LEFT, 18, y, 170, 20, -1);
    make(L"STATIC", L"Click a box, then press a key or a mouse button", SS_LEFT | SS_NOPREFIX, 196, y, 400, 20, -1);
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
         L"Mouse speed 1 is the normal speed; 2 turns twice as fast. Walk speed is how fast you move while holding "
         L"Walk (36 is the default). Keyboard and mouse changes also apply when you switch back to a running game.",
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
        tab.pszText = const_cast<wchar_t*>(kPageNames[i]);
        SendMessageW(g.tabs, TCM_INSERTITEMW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&tab));
    }

    define_fields();
    const wchar_t* const intros[kPageCount] = {
        L"How the game's window and picture look.",
        L"How smoothly the picture is shown. The game itself always runs at its original speed.",
        L"The volume of each kind of sound.",
        L"Voices, subtitles and the language of the game's text.",
        nullptr,
        L"Rumble and the camera stick. Which button does what, the trigger press and the button prompts are on the "
        L"Controller buttons page.",
        nullptr,
        L"Whether the game saves your progress by itself."};
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
    make(L"BUTTON", L"Restore defaults", BS_PUSHBUTTON | WS_TABSTOP, 680, kBottom + 16, 160, 32, kRestore);
    make(L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 850, kBottom + 16, 110, 32, kSave);
    make(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 968, kBottom + 16, 110, 32, kClose);
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
    show_message(L"This page is back to the game's defaults. Click Save to keep them.");
}

bool save() {
    if (!g.refused.empty()) {
        show_message(g.refused.begin()->second + L" Nothing was saved.");
        return false;
    }
    for (const std::string& problem : {check_display(g.display), check_options(g.options), check_controls(g.keys)})
        if (!problem.empty()) {
            show_message(wide(problem) + L" Nothing was saved.");
            return false;
        }
    std::string error;
    std::vector<std::wstring> saved;
    const auto fail = [&](const std::wstring& file) {
        std::wstring text = file + L" was not saved: " + wide(error);
        if (!saved.empty()) {
            text += L" (Saved:";
            for (const std::wstring& s : saved) text += L" " + s;
            text += L".)";
        }
        show_message(text);
        return false;
    };
    if (!(g.display == g.display_saved)) {
        if (!save_display(g.folder / "display.cfg", g.display_saved, g.display, &error)) return fail(L"display.cfg");
        g.display_saved = g.display;
        saved.push_back(L"display.cfg");
    }
    if (!(g.options == g.options_saved)) {
        if (!save_options(g.folder / "options.cfg", g.options_saved, g.options, &error)) return fail(L"options.cfg");
        g.options_saved = g.options;
        saved.push_back(L"options.cfg");
    }
    if (!same(g.binding, g.saved)) {
        if (!save_binding(g.folder / "pad.cfg", g.binding, &error)) return fail(L"pad.cfg");
        g.saved = g.binding;
        saved.push_back(L"pad.cfg");
    }
    if (!(g.keys == g.keys_saved)) {
        if (!save_controls(g.folder / "controls.cfg", g.keys_saved, g.keys, &error)) return fail(L"controls.cfg");
        g.keys_saved = g.keys;
        saved.push_back(L"controls.cfg");
    }
    show_message(saved.empty() ? L"Nothing to save: no setting was changed."
                               : L"Saved. The game uses the new settings the next time it starts.");
    return true;
}

bool confirm_close() {
    if (!unsaved() && g.refused.empty()) return true;
    const int answer = MessageBoxW(g.window, L"Save your changes before closing?", kTitle, MB_YESNOCANCEL | MB_ICONQUESTION);
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
        note(L"display.cfg has a line the game cannot read (" + wide(error) +
             L"), so the game will not start with it. Defaults are shown; fix or delete the file before saving.");
    }
    g.display_saved = g.display;
    if (!read_file(g.folder / "options.cfg", text, nullptr, &error)) note(wide(error));
    else read_options(text, g.options);
    g.options_saved = g.options;
    if (!read_file(g.folder / "controls.cfg", text, nullptr, &error)) note(wide(error));
    else read_controls(text, g.keys);
    g.keys_saved = g.keys;
    if (load_binding(g.folder / "pad.cfg", g.binding, &error) == LoadResult::Invalid)
        note(L"pad.cfg has a problem, so the default buttons are shown: " + wide(error));
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
    g.window = CreateWindowExW(0, cls.lpszClassName, kTitle, kStyleFlags, CW_USEDEFAULT, CW_USEDEFAULT,
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
