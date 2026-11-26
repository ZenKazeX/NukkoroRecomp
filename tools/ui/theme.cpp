#include "theme.h"

#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <string>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comctl32.lib")

namespace ui {

namespace {

Palette g_palette;
HBRUSH g_window = nullptr, g_field = nullptr, g_border = nullptr;
bool g_owned = false;  // the brushes above were made here (dark), not the system's (light)

// Windows' dark mode for menus, scroll bars and the lists of combo boxes has no published API. These uxtheme.dll
// functions, by ordinal, are what Windows' own programs use; they are present from Windows 10 1903 (build 18362) on, and
// nothing is called on older builds.
using SetPreferredAppModeFn = int(WINAPI*)(int);
using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);
using FlushMenuThemesFn = void(WINAPI*)();
struct DarkApi {
    SetPreferredAppModeFn set_preferred_app_mode = nullptr;
    AllowDarkModeForWindowFn allow_dark_mode_for_window = nullptr;
    FlushMenuThemesFn flush_menu_themes = nullptr;
};

DWORD windows_build() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
        if (auto get = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) get(&info);
    return info.dwMajorVersion >= 10 ? info.dwBuildNumber : 0;
}

const DarkApi& dark_api() {
    static const DarkApi api = [] {
        DarkApi a;
        if (windows_build() < 18362) return a;
        HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (uxtheme == nullptr) return a;
        a.allow_dark_mode_for_window = reinterpret_cast<AllowDarkModeForWindowFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(133)));
        a.set_preferred_app_mode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
        a.flush_menu_themes = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
        return a;
    }();
    return api;
}

void allow_dark(HWND h, bool dark) {
    if (dark_api().allow_dark_mode_for_window != nullptr) dark_api().allow_dark_mode_for_window(h, dark ? TRUE : FALSE);
}

bool high_contrast() {
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

std::wstring class_of(HWND h) {
    wchar_t name[64] = {};
    GetClassNameW(h, name, static_cast<int>(std::size(name)));
    return name;
}

bool is_class(HWND h, const wchar_t* name) { return lstrcmpiW(class_of(h).c_str(), name) == 0; }

std::wstring text_of(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring text(static_cast<std::size_t>(n) + 1, L'\0');
    GetWindowTextW(h, text.data(), n + 1);
    text.resize(static_cast<std::size_t>(n));
    return text;
}

void fill(HDC dc, const RECT& r, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void round_box(HDC dc, const RECT& r, COLORREF inside, COLORREF edge, int radius) {
    HBRUSH brush = CreateSolidBrush(inside);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
}

// ---- check boxes ----------------------------------------------------------------------------------------------------

LRESULT draw_check_box(const NMCUSTOMDRAW& cd) {
    if (cd.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    const Palette& p = g_palette;
    HWND h = cd.hdr.hwndFrom;
    HDC dc = cd.hdc;
    const RECT rc = cd.rc;
    fill(dc, rc, p.window);
    HGDIOBJ old_font = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    const int box = std::max(12, static_cast<int>(tm.tmHeight * 3 / 4));
    const bool checked = Button_GetCheck(h) == BST_CHECKED;
    const bool disabled = (cd.uItemState & CDIS_DISABLED) != 0 || !IsWindowEnabled(h);
    const bool hot = (cd.uItemState & CDIS_HOT) != 0;
    const int top = rc.top + (rc.bottom - rc.top - box) / 2;
    const RECT b{rc.left + 1, top, rc.left + 1 + box, top + box};
    if (checked) {
        const COLORREF on = disabled ? p.muted : p.accent;
        round_box(dc, b, on, on, box / 3);
        // The tick: Marlett's "a", the glyph Windows itself draws ticks with.
        HFONT marlett = CreateFontW(-(box - 2), 0, 0, 0, FW_NORMAL, 0, 0, 0, SYMBOL_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                                    L"Marlett");
        HGDIOBJ previous = SelectObject(dc, marlett);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, p.accent_text);
        RECT tick = b;
        DrawTextW(dc, L"a", 1, &tick, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, previous);
        DeleteObject(marlett);
    } else {
        round_box(dc, b, p.field, hot && !disabled ? p.text : p.border, box / 3);
    }
    const std::wstring text = text_of(h);
    RECT t{b.right + box / 2, rc.top, rc.right, rc.bottom};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? p.muted : p.text);
    const UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                        ((SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) != 0 ? DT_HIDEPREFIX : 0);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &t, format);
    if ((cd.uItemState & CDIS_FOCUS) != 0 && (SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0 && !text.empty()) {
        RECT f = t;
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &f, format | DT_CALCRECT);
        f.top = rc.top + (rc.bottom - rc.top - (f.bottom - f.top)) / 2 - 1;
        f.bottom = f.top + tm.tmHeight + 2;
        InflateRect(&f, 2, 0);
        SetTextColor(dc, p.text);
        DrawFocusRect(dc, &f);
    }
    SelectObject(dc, old_font);
    return CDRF_SKIPDEFAULT;
}

// ---- sliders ----------------------------------------------------------------------------------------------------------

LRESULT draw_slider(const NMCUSTOMDRAW& cd) {
    const Palette& p = g_palette;
    if (cd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd.dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    if (cd.dwItemSpec == TBCD_CHANNEL) {
        RECT r = cd.rc;
        const int middle = (r.top + r.bottom) / 2;
        r.top = middle - 2;
        r.bottom = middle + 2;
        round_box(cd.hdc, r, p.border, p.border, 4);
        return CDRF_SKIPDEFAULT;
    }
    if (cd.dwItemSpec == TBCD_THUMB) {
        const bool disabled = !IsWindowEnabled(cd.hdr.hwndFrom);
        const bool active = (cd.uItemState & (CDIS_HOT | CDIS_SELECTED)) != 0;
        const COLORREF knob = disabled ? p.muted : active ? p.text : p.accent;
        round_box(cd.hdc, cd.rc, knob, knob, std::max(2, static_cast<int>((cd.rc.right - cd.rc.left) / 2)));
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

// ---- the tab strip (Windows has no dark tab control) -----------------------------------------------------------------

LRESULT CALLBACK tab_proc(HWND h, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
    if (!g_palette.dark) return DefSubclassProc(h, message, wparam, lparam);
    if (message == WM_ERASEBKGND) return 1;
    if (message != WM_PAINT && message != WM_PRINTCLIENT) return DefSubclassProc(h, message, wparam, lparam);
    const Palette& p = g_palette;
    PAINTSTRUCT ps{};
    HDC dc = message == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(wparam);
    RECT rc;
    GetClientRect(h, &rc);
    fill(dc, rc, p.window);
    HGDIOBJ old_font = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
    SetBkMode(dc, TRANSPARENT);
    const int count = TabCtrl_GetItemCount(h), chosen = TabCtrl_GetCurSel(h);
    int bottom = rc.bottom;
    for (int i = 0; i < count; ++i) {
        RECT r{};
        TabCtrl_GetItemRect(h, i, &r);
        bottom = r.bottom;
        wchar_t text[128] = {};
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = text;
        item.cchTextMax = static_cast<int>(std::size(text));
        SendMessageW(h, TCM_GETITEMW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&item));  // the wide form in any build
        if (i == chosen) {
            RECT face = r;
            InflateRect(&face, 2, 2);
            face.bottom = r.bottom;
            fill(dc, face, p.field);
            RECT bar{face.left, face.bottom - 3, face.right, face.bottom};
            fill(dc, bar, p.accent);
        }
        SetTextColor(dc, i == chosen ? p.text : p.muted);
        DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (i == chosen && GetFocus() == h && (SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0) {
            RECT f = r;
            InflateRect(&f, -3, -3);
            SetTextColor(dc, p.text);
            DrawFocusRect(dc, &f);
        }
    }
    RECT line{rc.left, std::min(bottom, static_cast<int>(rc.bottom)) - 1, rc.right, std::min(bottom, static_cast<int>(rc.bottom))};
    fill(dc, line, p.border);
    SelectObject(dc, old_font);
    if (message == WM_PAINT) EndPaint(h, &ps);
    return 0;
}

// ---- lists: the column headings' text (their custom draw goes to the list, not to the program) ----------------------

LRESULT CALLBACK list_proc(HWND h, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
    if (message == WM_NOTIFY && g_palette.dark) {
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header->code == NM_CUSTOMDRAW && header->hwndFrom == ListView_GetHeader(h)) {
            const auto* cd = reinterpret_cast<const NMCUSTOMDRAW*>(lparam);
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                SetTextColor(cd->hdc, g_palette.text);
                return CDRF_DODEFAULT;
            }
        }
    }
    return DefSubclassProc(h, message, wparam, lparam);
}

void set_theme(HWND h, bool dark, const wchar_t* dark_name, const wchar_t* light_name = nullptr) {
    allow_dark(h, dark);
    SetWindowTheme(h, dark ? dark_name : light_name, nullptr);
}

}  // namespace

bool windows_prefers_dark() {
    if (high_contrast()) return false;
    DWORD light = 1, size = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS)
        return false;
    return light == 0;
}

bool is_dark(Theme theme) {
    if (high_contrast()) return false;
    return theme == Theme::Dark || (theme == Theme::System && windows_prefers_dark());
}

void set_dark(bool dark) {
    if (g_owned) {
        DeleteObject(g_window);
        DeleteObject(g_field);
        DeleteObject(g_border);
    }
    Palette& p = g_palette;
    p.dark = dark;
    if (dark) {
        p.window = RGB(32, 32, 32);
        p.text = RGB(242, 242, 242);
        p.muted = RGB(178, 178, 178);
        p.field = RGB(48, 48, 48);
        p.field_text = RGB(242, 242, 242);
        p.border = RGB(110, 110, 110);
        p.accent = RGB(96, 170, 255);
        p.accent_text = RGB(0, 0, 0);
        g_window = CreateSolidBrush(p.window);
        g_field = CreateSolidBrush(p.field);
        g_border = CreateSolidBrush(p.border);
        g_owned = true;
    } else {
        p.window = GetSysColor(COLOR_BTNFACE);
        p.text = GetSysColor(COLOR_BTNTEXT);
        p.muted = GetSysColor(COLOR_GRAYTEXT);
        p.field = GetSysColor(COLOR_WINDOW);
        p.field_text = GetSysColor(COLOR_WINDOWTEXT);
        p.border = GetSysColor(COLOR_3DSHADOW);
        p.accent = GetSysColor(COLOR_HIGHLIGHT);
        p.accent_text = GetSysColor(COLOR_HIGHLIGHTTEXT);
        g_window = GetSysColorBrush(COLOR_BTNFACE);
        g_field = GetSysColorBrush(COLOR_WINDOW);
        g_border = GetSysColorBrush(COLOR_3DSHADOW);
        g_owned = false;
    }
    const DarkApi& api = dark_api();
    if (api.set_preferred_app_mode != nullptr) api.set_preferred_app_mode(dark ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
    if (api.flush_menu_themes != nullptr) api.flush_menu_themes();
}

const Palette& colors() { return g_palette; }
HBRUSH window_brush() { return g_window != nullptr ? g_window : GetSysColorBrush(COLOR_BTNFACE); }
HBRUSH field_brush() { return g_field != nullptr ? g_field : GetSysColorBrush(COLOR_WINDOW); }

void apply_to_window(HWND window) {
    const bool dark = g_palette.dark;
    const BOOL value = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(window, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &value, sizeof(value));
    set_theme(window, dark, L"DarkMode_Explorer");
    EnumChildWindows(
        window,
        [](HWND child, LPARAM) -> BOOL {
            apply_to_control(child);
            return TRUE;
        },
        0);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void apply_to_control(HWND h) {
    const bool dark = g_palette.dark;
    if (is_class(h, WC_BUTTONW)) {
        set_theme(h, dark, L"DarkMode_Explorer");
    } else if (is_class(h, WC_COMBOBOXW)) {
        set_theme(h, dark, L"DarkMode_CFD");
        COMBOBOXINFO info{};
        info.cbSize = sizeof(info);
        if (GetComboBoxInfo(h, &info)) {
            if (info.hwndList != nullptr) set_theme(info.hwndList, dark, L"DarkMode_Explorer");
            if (info.hwndItem != nullptr && info.hwndItem != h) set_theme(info.hwndItem, dark, L"DarkMode_CFD");
        }
    } else if (is_class(h, WC_EDITW)) {
        set_theme(h, dark, L"DarkMode_CFD");
    } else if (is_class(h, WC_LISTVIEWW)) {
        set_theme(h, dark, L"DarkMode_Explorer", L"Explorer");
        ListView_SetBkColor(h, g_palette.field);
        ListView_SetTextBkColor(h, g_palette.field);
        ListView_SetTextColor(h, g_palette.field_text);
        if (HWND header = ListView_GetHeader(h)) set_theme(header, dark, L"DarkMode_ItemsView");
        SetWindowSubclass(h, list_proc, 1, 0);
        // The check boxes are pictures made from the theme when the style is set: set it again to remake them.
        const DWORD style = ListView_GetExtendedListViewStyle(h);
        if ((style & LVS_EX_CHECKBOXES) != 0) {
            ListView_SetExtendedListViewStyle(h, style & ~static_cast<DWORD>(LVS_EX_CHECKBOXES));
            ListView_SetExtendedListViewStyle(h, style);
        }
    } else if (is_class(h, WC_TABCONTROLW)) {
        SetWindowSubclass(h, tab_proc, 1, 0);
    }
    InvalidateRect(h, nullptr, TRUE);
}

LRESULT control_color(UINT message, HDC dc, HWND control, bool muted) {
    const Palette& p = g_palette;
    const bool field = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX ||
                       (p.dark && message == WM_CTLCOLORSTATIC && is_class(control, WC_EDITW));
    if (field) {
        SetTextColor(dc, p.field_text);
        SetBkColor(dc, p.field);
        return reinterpret_cast<LRESULT>(field_brush());
    }
    SetTextColor(dc, muted ? p.muted : p.text);
    SetBkColor(dc, p.window);
    return reinterpret_cast<LRESULT>(window_brush());
}

bool custom_draw(const NMHDR* header, LRESULT* result) {
    if (!g_palette.dark || header->code != NM_CUSTOMDRAW) return false;
    const auto& cd = *reinterpret_cast<const NMCUSTOMDRAW*>(header);
    if (is_class(header->hwndFrom, WC_BUTTONW)) {
        const LONG type = GetWindowLongW(header->hwndFrom, GWL_STYLE) & BS_TYPEMASK;
        if (type != BS_CHECKBOX && type != BS_AUTOCHECKBOX) return false;
        *result = draw_check_box(cd);
        return true;
    }
    if (is_class(header->hwndFrom, TRACKBAR_CLASSW)) {
        *result = draw_slider(cd);
        return true;
    }
    return false;
}

int font_height(int points, int dpi, int text_size) { return -MulDiv(points * text_size, dpi, 72 * 100); }

}  // namespace ui
