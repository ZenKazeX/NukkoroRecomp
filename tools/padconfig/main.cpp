// ShadowPadConfig.exe: maps controller buttons to the game's buttons and saves pad.cfg beside this program, which is
// where the game reads it. Click a row, then press the controller button. The game reads pad.cfg when it starts.

#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <xinput.h>

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

#include <array>
#include <filesystem>
#include <string>

#include "controller_view.h"
#include "pad_config.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

using namespace padconfig;
namespace sp = shadow::peripheral;

constexpr int kFirstRow = 100, kFirstLive = 200;
enum : int { kThreshold = 300, kThresholdValue, kSwapSticks, kStyle, kDefaults, kSave, kClose, kStatus };
constexpr UINT_PTR kPollTimer = 1;
constexpr DWORD kCaptureMs = 6000;

const struct {
    const wchar_t* label;
    sp::ButtonStyle style;
} kStyles[] = {{L"Automatic", sp::ButtonStyle::Auto},
               {L"Original (GameCube)", sp::ButtonStyle::Original},
               {L"Xbox", sp::ButtonStyle::Xbox},
               {L"PlayStation", sp::ButtonStyle::PlayStation},
               {L"Nintendo Switch", sp::ButtonStyle::Switch}};

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

struct App {
    HWND window = nullptr;
    HFONT font = nullptr;
    int dpi = 96;
    std::filesystem::path path;
    GamepadBinding binding, saved;
    GetStateFn get_state = nullptr;
    int capturing = -1;
    DWORD capture_end = 0;
    WORD held = 0;
    bool connected = false, status_is_message = false;
    std::wstring message;
    ControllerView view;
    bool left_trigger = false, right_trigger = false;
} g;

std::wstring wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
int px(int v) { return MulDiv(v, g.dpi, 96); }
HWND item(int id) { return GetDlgItem(g.window, id); }

void set_text(HWND h, const std::wstring& text) {
    wchar_t now[128] = {};
    GetWindowTextW(h, now, 128);
    if (text != now) SetWindowTextW(h, text.c_str());
}

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

// What a row reads as. The analog triggers always work as L and R (peripheral/gamepad_binding.h), whatever else is set.
std::wstring row_text(std::size_t i) { return control_label(g.binding, i); }

void refresh_rows() {
    for (std::size_t i = 0; i < kControls.size(); ++i) {
        const bool waiting = g.capturing == static_cast<int>(i);
        set_text(item(kFirstRow + static_cast<int>(i)), waiting ? L"Press a button..." : row_text(i));
    }
}

void refresh_status() {
    std::wstring text;
    if (g.capturing >= 0) {
        text = L"Press the controller button for " + wide(kControls[static_cast<std::size_t>(g.capturing)].name) +
               L". Esc cancels, Delete clears it.";
    } else if (g.status_is_message) {
        text = g.message;
    } else if (!g.connected) {
        text = L"No controller found. Connect one to choose buttons.";
    } else if (!same(g.binding, g.saved)) {
        text = L"Unsaved changes.";
    } else {
        text = L"Controller connected. Changes apply the next time the game starts.";
    }
    set_text(item(kStatus), text);
}

void show_message(const std::wstring& text) {
    g.status_is_message = true;
    g.message = text;
    refresh_status();
}

void refresh_options() {
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
    return RECT{px(414), px(12), px(1082), px(610)};
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
    for (std::size_t i = 0; i < kControls.size(); ++i) {
        const std::uint16_t mask = g.binding.*kControls[i].field;
        const bool down = g.capturing < 0 && mask != 0u && (buttons & mask) != 0u;
        set_text(item(kFirstLive + static_cast<int>(i)), down ? L"●" : L"");
    }
    refresh_status();
    refresh_picture(buttons);
}

bool save() {
    std::string error;
    if (!save_binding(g.path, g.binding, &error)) {
        show_message(wide(error));
        return false;
    }
    g.saved = g.binding;
    show_message(L"Saved. The game uses this the next time it starts.");
    return true;
}

bool confirm_close() {
    if (same(g.binding, g.saved)) return true;
    const int answer = MessageBoxW(g.window, L"Save your changes before closing?", L"Controller Buttons", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL) return false;
    return answer == IDNO || save();
}

HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), g.window,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return c;
}

void build() {
    constexpr int kRowH = 32, kLeft = 18;
    int y = 14;
    make(L"STATIC", L"Game button", SS_LEFT, kLeft, y, 130, 20, -1);
    make(L"STATIC", L"Your controller (click to change)", SS_LEFT, 150, y, 260, 20, -1);
    y += 26;
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
    y += kRowH + 4;
    make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, kLeft, y, 380, 38, kStatus);
    y += 44;
    make(L"BUTTON", L"Defaults", BS_PUSHBUTTON | WS_TABSTOP, kLeft, y, 90, 30, kDefaults);
    make(L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 212, y, 86, 30, kSave);
    make(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 304, y, 86, 30, kClose);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(hwnd, &paint);
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
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_TIMER:
        if (wparam == kPollTimer) on_timer();
        return 0;
    case WM_LBUTTONDOWN: {
        const int control = control_at(picture_area(), POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        if (control >= 0) start_capture(control);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            POINT point;
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            if (control_at(picture_area(), point) >= 0) {
                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
                return TRUE;
            }
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lparam) == item(kThreshold)) {
            g.status_is_message = false;
            g.binding.trigger_threshold = static_cast<std::uint8_t>(SendMessageW(item(kThreshold), TBM_GETPOS, 0, 0));
            set_text(item(kThresholdValue), std::to_wstring(g.binding.trigger_threshold));
            refresh_status();
        }
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id >= kFirstRow && id < kFirstRow + static_cast<int>(kControls.size())) {
            if (HIWORD(wparam) == BN_CLICKED) start_capture(id - kFirstRow);
        } else if (id == kSwapSticks) {
            g.status_is_message = false;
            g.binding.swap_sticks = IsDlgButtonChecked(hwnd, kSwapSticks) == BST_CHECKED;
            refresh_status();
        } else if (id == kStyle && HIWORD(wparam) == CBN_SELCHANGE) {
            g.status_is_message = false;
            g.binding.button_style = kStyles[SendMessageW(item(kStyle), CB_GETCURSEL, 0, 0)].style;
            refresh_status();
        } else if (id == kDefaults) {
            g.capturing = -1;
            g.status_is_message = false;
            g.binding = GamepadBinding{};
            refresh_rows();
            refresh_options();
            refresh_status();
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

std::filesystem::path beside_this_program() {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path(path).parent_path() / "pad.cfg";
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    Gdiplus::GdiplusStartupInput gdiplus_input;
    ULONG_PTR gdiplus_token = 0;
    Gdiplus::GdiplusStartup(&gdiplus_token, &gdiplus_input, nullptr);
    load_xinput();
    HDC screen = GetDC(nullptr);
    g.dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    g.font = CreateFontW(-MulDiv(10, g.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    g.path = beside_this_program();
    std::string error;
    const LoadResult loaded = load_binding(g.path, g.binding, &error);
    g.saved = g.binding;
    g.view.binding = g.binding;

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
    RECT size{0, 0, px(1094), px(620)};
    AdjustWindowRect(&size, kStyleFlags, FALSE);
    g.window = CreateWindowExW(0, cls.lpszClassName, L"Controller Buttons - Shadow the Hedgehog", kStyleFlags, CW_USEDEFAULT, CW_USEDEFAULT,
                               size.right - size.left, size.bottom - size.top, nullptr, nullptr, instance, nullptr);
    build();
    refresh_rows();
    refresh_options();
    if (loaded == LoadResult::Invalid) show_message(L"pad.cfg has a problem, so the defaults are shown: " + wide(error));
    refresh_status();
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
        if (IsDialogMessageW(g.window, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Gdiplus::GdiplusShutdown(gdiplus_token);
    return 0;
}
