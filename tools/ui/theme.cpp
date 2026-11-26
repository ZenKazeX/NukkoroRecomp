#include "theme.h"

#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

#include <iterator>
#include <string>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace ui {

namespace {

Palette g_palette;
HBRUSH g_window = nullptr, g_surface = nullptr, g_field = nullptr;
bool g_owned = false;  // the brushes above were made here, not the system's
ULONG_PTR g_gdiplus = 0;

const wchar_t* const kPrimary = L"ui.primary";
const wchar_t* const kSwitch = L"ui.switch";
const wchar_t* const kIcon = L"ui.icon";
const wchar_t* const kIconOnly = L"ui.icon_only";

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
    static const DWORD build = [] {
        OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
            if (auto get = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) get(&info);
        return info.dwMajorVersion >= 10 ? info.dwBuildNumber : 0ul;
    }();
    return build;
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

bool font_installed(const wchar_t* face) {
    LOGFONTW want{};
    want.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(want.lfFaceName, face, LF_FACESIZE);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(
        dc, &want, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found_flag) -> int {
            *reinterpret_cast<bool*>(found_flag) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

double luminance(COLORREF c) {
    return (0.2126 * GetRValue(c) + 0.7152 * GetGValue(c) + 0.0722 * GetBValue(c)) / 255.0;
}

// Windows' accent colour in the shade its own controls use: a darker one on light, a lighter one on dark
// (Explorer\Accent\AccentPalette: eight colours, lightest first).
COLORREF windows_accent(bool dark) {
    BYTE data[32] = {};
    DWORD size = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentPalette",
                     RRF_RT_REG_BINARY, nullptr, data, &size) == ERROR_SUCCESS &&
        size >= 28) {
        const int i = dark ? 1 : 4;
        return RGB(data[i * 4], data[i * 4 + 1], data[i * 4 + 2]);
    }
    return dark ? RGB(0x60, 0xCD, 0xFF) : RGB(0x00, 0x5F, 0xB8);
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

bool focus_shown(HWND h) { return (SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0; }

// ---- anti-aliased shapes (GDI+) ---------------------------------------------------------------------------------------

Gdiplus::Color gp(COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }

void rounded_path(Gdiplus::GraphicsPath& path, Gdiplus::RectF r, float radius) {
    radius = std::min(radius, std::min(r.Width, r.Height) / 2.0f);
    const float d = radius * 2.0f;
    if (d <= 0.0f) {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

// A rounded rectangle inside `r` (pixel edges), filled and edged; `stroke` 0: no edge.
void round_rect(HDC dc, const RECT& r, float radius, COLORREF fill, COLORREF edge, float stroke = 1.0f) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const float inset = stroke / 2.0f;
    Gdiplus::RectF box(r.left + inset, r.top + inset, (r.right - r.left) - stroke, (r.bottom - r.top) - stroke);
    Gdiplus::GraphicsPath path;
    rounded_path(path, box, radius);
    Gdiplus::SolidBrush brush(gp(fill));
    g.FillPath(&brush, &path);
    if (stroke > 0.0f) {
        Gdiplus::Pen pen(gp(edge), stroke);
        g.DrawPath(&pen, &path);
    }
}

void circle(HDC dc, float cx, float cy, float radius, COLORREF fill) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::SolidBrush brush(gp(fill));
    g.FillEllipse(&brush, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
}

void tick(HDC dc, const RECT& box, COLORREF color) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float w = static_cast<float>(box.right - box.left), x = static_cast<float>(box.left), y = static_cast<float>(box.top);
    Gdiplus::Pen pen(gp(color), std::max(1.5f, w / 9.0f));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    const Gdiplus::PointF points[] = {{x + w * 0.25f, y + w * 0.52f}, {x + w * 0.43f, y + w * 0.70f}, {x + w * 0.76f, y + w * 0.33f}};
    g.DrawLines(&pen, points, 3);
}

// The control's background as its parent paints it (on the window or on a card).
void fill_parent_background(HWND h, HDC dc, const RECT& r) {
    auto brush = reinterpret_cast<HBRUSH>(SendMessageW(GetParent(h), WM_CTLCOLORBTN, reinterpret_cast<WPARAM>(dc),
                                                       reinterpret_cast<LPARAM>(h)));
    FillRect(dc, &r, brush != nullptr ? brush : g_window);
}

void focus_ring(HDC dc, const RECT& r, float radius) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    rounded_path(path, Gdiplus::RectF(r.left + 1.0f, r.top + 1.0f, r.right - r.left - 2.0f, r.bottom - r.top - 2.0f), radius);
    Gdiplus::Pen pen(gp(g_palette.text), 2.0f);
    g.DrawPath(&pen, &path);
}

HFONT icon_font_for(HDC dc, int height) {
    if (*icon_face() == L'\0') return nullptr;
    (void)dc;
    return CreateFontW(-height, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, icon_face());
}

// ---- push buttons -------------------------------------------------------------------------------------------------------

LRESULT draw_button(const NMCUSTOMDRAW& cd) {
    if (cd.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    const Palette& p = g_palette;
    HWND h = cd.hdr.hwndFrom;
    HDC dc = cd.hdc;
    const RECT rc = cd.rc;
    fill_parent_background(h, dc, rc);
    const bool disabled = !IsWindowEnabled(h);
    const bool hot = (cd.uItemState & CDIS_HOT) != 0, pressed = (cd.uItemState & CDIS_SELECTED) != 0;
    const bool primary = GetPropW(h, kPrimary) != nullptr;
    HGDIOBJ old_font = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    const float radius = std::max(4.0f, tm.tmHeight / 4.0f);
    RECT face = rc;
    InflateRect(&face, -1, -1);
    COLORREF fill = p.control, edge = p.control_stroke, ink = p.text;
    if (primary && !disabled) {
        fill = p.accent;
        edge = p.accent;
        ink = p.accent_text;
        if (pressed) fill = RGB(GetRValue(p.accent) * 85 / 100, GetGValue(p.accent) * 85 / 100, GetBValue(p.accent) * 85 / 100);
        else if (hot) fill = RGB(std::min(255, GetRValue(p.accent) + 18), std::min(255, GetGValue(p.accent) + 18), std::min(255, GetBValue(p.accent) + 18));
    } else {
        if (pressed) fill = p.control_pressed;
        else if (hot) fill = p.control_hot;
        if (disabled) ink = p.muted;
    }
    round_rect(dc, face, radius, fill, edge);
    const std::wstring text = text_of(h);
    RECT t = face;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ink);
    const auto glyph = static_cast<wchar_t>(reinterpret_cast<UINT_PTR>(GetPropW(h, kIcon)));
    HFONT icons = glyph != 0 ? icon_font_for(dc, tm.tmHeight * 9 / 10) : nullptr;
    if (icons != nullptr) {
        // The icon and the text together, centred; only the icon when the button is too narrow for its words (its
        // text stays its name for screen readers and its tooltip).
        const bool icon_only = GetPropW(h, kIconOnly) != nullptr;
        RECT measure{0, 0, 0, 0};
        if (!icon_only)
            DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &measure, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        const int icon_w = tm.tmHeight, gap = text.empty() || icon_only ? 0 : tm.tmHeight / 2;
        const int total = icon_w + gap + (measure.right - measure.left);
        RECT icon_box{face.left + (face.right - face.left - total) / 2, face.top, 0, face.bottom};
        icon_box.right = icon_box.left + icon_w;
        HGDIOBJ previous = SelectObject(dc, icons);
        DrawTextW(dc, &glyph, 1, &icon_box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, previous);
        DeleteObject(icons);
        t.left = icon_box.right + gap;
        if (!icon_only)
            DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_HIDEPREFIX);
    } else {
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &t,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_HIDEPREFIX);
    }
    if ((cd.uItemState & CDIS_FOCUS) != 0 && focus_shown(h)) {
        RECT ring = rc;
        focus_ring(dc, ring, radius + 1.0f);
    }
    SelectObject(dc, old_font);
    return CDRF_SKIPDEFAULT;
}

// ---- check boxes and switches -----------------------------------------------------------------------------------------

LRESULT draw_check_box(const NMCUSTOMDRAW& cd) {
    if (cd.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    const Palette& p = g_palette;
    HWND h = cd.hdr.hwndFrom;
    HDC dc = cd.hdc;
    const RECT rc = cd.rc;
    fill_parent_background(h, dc, rc);
    HGDIOBJ old_font = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    const bool checked = Button_GetCheck(h) == BST_CHECKED;
    const bool disabled = (cd.uItemState & CDIS_DISABLED) != 0 || !IsWindowEnabled(h);
    const bool hot = (cd.uItemState & CDIS_HOT) != 0;
    const COLORREF on = disabled ? p.muted : p.accent;
    int text_left = 0;
    if (GetPropW(h, kSwitch) != nullptr) {
        // An on/off switch: a pill, with its knob at the right when on.
        const int height = std::max(14, static_cast<int>(tm.tmHeight)), width = height * 2;
        const int top = rc.top + (rc.bottom - rc.top - height) / 2;
        const RECT pill{rc.left + 1, top, rc.left + 1 + width, top + height};
        const float knob = height * (hot && !disabled ? 0.34f : 0.28f);
        const float cy = (pill.top + pill.bottom) / 2.0f;
        if (checked) {
            round_rect(dc, pill, height / 2.0f, on, on);
            circle(dc, pill.right - height / 2.0f, cy, knob, p.accent_text);
        } else {
            const COLORREF edge = disabled ? p.muted : p.border;
            round_rect(dc, pill, height / 2.0f, hot && !disabled ? p.control_hot : p.control, edge);
            circle(dc, pill.left + height / 2.0f, cy, knob, edge);
        }
        text_left = pill.right + height / 2;
    } else {
        const int box = std::max(12, static_cast<int>(tm.tmHeight * 4 / 5));
        const int top = rc.top + (rc.bottom - rc.top - box) / 2;
        const RECT b{rc.left + 1, top, rc.left + 1 + box, top + box};
        if (checked) {
            round_rect(dc, b, box / 4.0f, on, on);
            tick(dc, b, p.accent_text);
        } else {
            round_rect(dc, b, box / 4.0f, hot && !disabled ? p.control_hot : p.control, disabled ? p.muted : p.border);
        }
        text_left = b.right + box / 2;
    }
    const std::wstring text = text_of(h);
    RECT t{text_left, rc.top, rc.right, rc.bottom};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? p.muted : p.text);
    // BS_MULTILINE: the words wrap to the box's width, the lines centred on its height.
    const bool multiline = (GetWindowLongW(h, GWL_STYLE) & BS_MULTILINE) != 0;
    UINT format = DT_LEFT | DT_HIDEPREFIX | (multiline ? DT_WORDBREAK : DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (multiline) {
        RECT need = t;
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &need, format | DT_CALCRECT);
        t.top = rc.top + std::max(0L, (rc.bottom - rc.top - (need.bottom - need.top)) / 2);
    }
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &t, format);
    if ((cd.uItemState & CDIS_FOCUS) != 0 && focus_shown(h)) {
        RECT f = t;
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &f, format | DT_CALCRECT);
        f.left = rc.left;
        f.top = multiline ? std::max(rc.top + 1, f.top - 2) : rc.top + 1;
        f.bottom = multiline ? std::min(rc.bottom - 1, f.bottom + 2) : rc.bottom - 1;
        f.right = std::min(static_cast<int>(rc.right), static_cast<int>(f.right) + 3);
        focus_ring(dc, f, 4.0f);
    }
    SelectObject(dc, old_font);
    return CDRF_SKIPDEFAULT;
}

// ---- sliders: a thin track, filled in the accent up to a round knob -------------------------------------------------

LRESULT draw_slider(const NMCUSTOMDRAW& cd) {
    const Palette& p = g_palette;
    if (cd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd.dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    HWND h = cd.hdr.hwndFrom;
    const bool disabled = !IsWindowEnabled(h);
    RECT thumb{};
    SendMessageW(h, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    const float cx = (thumb.left + thumb.right) / 2.0f;
    if (cd.dwItemSpec == TBCD_CHANNEL) {
        RECT r = cd.rc;
        const int middle = (r.top + r.bottom) / 2;
        r.top = middle - 2;
        r.bottom = middle + 2;
        round_rect(cd.hdc, r, 2.0f, p.border, p.border, 0.0f);
        RECT done = r;
        done.right = static_cast<LONG>(cx);
        if (done.right > done.left) round_rect(cd.hdc, done, 2.0f, disabled ? p.muted : p.accent, p.accent, 0.0f);
        return CDRF_SKIPDEFAULT;
    }
    if (cd.dwItemSpec == TBCD_THUMB) {
        const bool active = (cd.uItemState & (CDIS_HOT | CDIS_SELECTED)) != 0;
        const float cy = (thumb.top + thumb.bottom) / 2.0f;
        const float outer = std::min(thumb.right - thumb.left, thumb.bottom - thumb.top) / 2.0f + 2.0f;
        circle(cd.hdc, cx, cy, outer, p.control_stroke);
        circle(cd.hdc, cx, cy, outer - 1.0f, p.control);
        circle(cd.hdc, cx, cy, outer * (active ? 0.58f : 0.45f), disabled ? p.muted : p.accent);
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

// ---- the tab strip (Windows has no dark tab control) -----------------------------------------------------------------

LRESULT CALLBACK tab_proc(HWND h, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
    if (!g_palette.custom || !g_palette.dark) return DefSubclassProc(h, message, wparam, lparam);
    if (message == WM_ERASEBKGND) return 1;
    if (message != WM_PAINT && message != WM_PRINTCLIENT) return DefSubclassProc(h, message, wparam, lparam);
    const Palette& p = g_palette;
    PAINTSTRUCT ps{};
    HDC dc = message == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(wparam);
    RECT rc;
    GetClientRect(h, &rc);
    FillRect(dc, &rc, g_window);
    HGDIOBJ old_font = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
    SetBkMode(dc, TRANSPARENT);
    const int count = TabCtrl_GetItemCount(h), chosen = TabCtrl_GetCurSel(h);
    for (int i = 0; i < count; ++i) {
        RECT r{};
        TabCtrl_GetItemRect(h, i, &r);
        wchar_t text[128] = {};
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = text;
        item.cchTextMax = static_cast<int>(std::size(text));
        SendMessageW(h, TCM_GETITEMW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&item));
        if (i == chosen) {
            RECT bar{r.left + 6, r.bottom - 3, r.right - 6, r.bottom};
            round_rect(dc, bar, 1.5f, p.accent, p.accent, 0.0f);
        }
        SetTextColor(dc, i == chosen ? p.text : p.muted);
        DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, old_font);
    if (message == WM_PAINT) EndPaint(h, &ps);
    return 0;
}

// ---- lists: the column headings' text (their custom draw goes to the list, not to the program) ----------------------

LRESULT CALLBACK list_proc(HWND h, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
    if (message == WM_NOTIFY && g_palette.custom && g_palette.dark) {
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

// ---- drop-down lists: a rounded box with a chevron, and roomy rows with a rounded highlight -----------------------------

const wchar_t* const kHot = L"ui.hot";

int font_line(HFONT font) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return tm.tmHeight;
}

HFONT combo_font(HWND h) {
    auto font = reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0));
    return font != nullptr ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

bool editable_combo(HWND h) { return (GetWindowLongW(h, GWL_STYLE) & 3) == CBS_DROPDOWN; }

std::wstring combo_item_text(HWND h, int index) {
    if (index < 0) return {};
    const int n = static_cast<int>(SendMessageW(h, CB_GETLBTEXTLEN, static_cast<WPARAM>(index), 0));
    if (n <= 0) return {};
    std::wstring text(static_cast<std::size_t>(n) + 1, L'\0');
    SendMessageW(h, CB_GETLBTEXT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<std::size_t>(n));
    return text;
}

// A downward chevron centred on (cx, cy), `size` wide.
void chevron(HDC dc, float cx, float cy, float size, COLORREF color) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(gp(color), std::max(1.2f, size / 7.0f));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    const float half = size / 2.0f, rise = size / 4.0f;
    const Gdiplus::PointF points[] = {{cx - half, cy - rise}, {cx, cy + rise}, {cx + half, cy - rise}};
    g.DrawLines(&pen, points, 3);
}

// The closed box, drawn whole (off screen first): a rounded face like a button's, the chosen item's words, the chevron.
// An editable one is a field with its own edit inside: only its face, edge and chevron are drawn here.
void paint_dropdown(HWND h, HDC target) {
    const Palette& p = g_palette;
    RECT rc;
    GetClientRect(h, &rc);
    const int w = rc.right - rc.left, hgt = rc.bottom - rc.top;
    if (w <= 0 || hgt <= 0) return;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, w, hgt);
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    fill_parent_background(h, dc, rc);
    HFONT font = combo_font(h);
    HGDIOBJ old_font = SelectObject(dc, font);
    const int line = font_line(font);
    const float radius = std::max(4.0f, line / 4.0f);
    const bool editable = editable_combo(h);
    const bool disabled = !IsWindowEnabled(h);
    const bool dropped = SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0) != FALSE;
    const bool hot = GetPropW(h, kHot) != nullptr;
    HWND focus = GetFocus();
    const bool focused = focus == h || (focus != nullptr && GetParent(focus) == h);
    COLORREF fill = editable ? p.field : dropped ? p.control_pressed : hot && !disabled ? p.control_hot : p.control;
    RECT face = rc;
    round_rect(dc, face, radius, fill, p.control_stroke);
    // The chevron in the last line and a fifth (an editable box's edit ends before it: place_edit()).
    const int arrow_left = rc.right - line * 6 / 5 - 4;
    const COLORREF ink = disabled ? p.muted : p.text;
    chevron(dc, (arrow_left + rc.right) / 2.0f, hgt / 2.0f, line * 0.55f, ink);
    if (!editable) {
        const std::wstring text = combo_item_text(h, static_cast<int>(SendMessageW(h, CB_GETCURSEL, 0, 0)));
        RECT t{rc.left + line / 2, rc.top, arrow_left, rc.bottom};
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ink);
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (focused && focus_shown(h)) focus_ring(dc, rc, radius + 1.0f);
    } else if (focused) {
        // A field with the focus: Windows 11's accent line along its bottom.
        RECT underline{rc.left + 1, rc.bottom - 2, rc.right - 1, rc.bottom};
        SetDCBrushColor(dc, p.accent);
        FillRect(dc, &underline, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    BitBlt(target, 0, 0, w, hgt, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_font);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

// An editable box's edit: its words at the same place as a closed list's, centred on the box's height, clear of the
// chevron. Windows places it again whenever the box changes size or font: this follows.
void place_edit(HWND h) {
    COMBOBOXINFO info{};
    info.cbSize = sizeof(info);
    if (!editable_combo(h) || !GetComboBoxInfo(h, &info) || info.hwndItem == nullptr || info.hwndItem == h) return;
    RECT rc;
    GetClientRect(h, &rc);
    const int line = font_line(combo_font(h));
    const int edit_h = std::min(line + 2, static_cast<int>(rc.bottom) - 4);
    const int left = line / 2 - 1, right = rc.right - line * 6 / 5 - 6;
    SetWindowPos(info.hwndItem, nullptr, left, (rc.bottom - edit_h) / 2, std::max(1, right - left), edit_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK dropdown_proc(HWND h, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
    // The mouse wheel over a closed list scrolls the page it is on, as in Windows 11's Settings; it does not change
    // the choice (scrolling past a list must not change a setting). An open list still scrolls itself.
    if (message == WM_MOUSEWHEEL && SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0) == FALSE)
        return SendMessageW(GetParent(h), message, wparam, lparam);
    if (!g_palette.custom) return DefSubclassProc(h, message, wparam, lparam);
    switch (message) {
        case WM_SIZE:
        case WM_SETFONT:
        case CB_SETITEMHEIGHT: {
            const LRESULT result = DefSubclassProc(h, message, wparam, lparam);
            place_edit(h);
            return result;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            paint_dropdown(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            paint_dropdown(h, reinterpret_cast<HDC>(wparam));
            return 0;
        case WM_MOUSEMOVE:
            if (GetPropW(h, kHot) == nullptr) {
                SetPropW(h, kHot, reinterpret_cast<HANDLE>(1));
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
                TrackMouseEvent(&track);
                InvalidateRect(h, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            RemovePropW(h, kHot);
            InvalidateRect(h, nullptr, FALSE);
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
        case CB_SETCURSEL:
        case CB_SHOWDROPDOWN:
        case WM_COMMAND:  // from an editable box's edit: its focus
        case WM_CTLCOLOREDIT: {
            const LRESULT result = DefSubclassProc(h, message, wparam, lparam);
            if (message != WM_CTLCOLOREDIT) InvalidateRect(h, nullptr, FALSE);
            return result;
        }
        case WM_NCDESTROY:
            RemovePropW(h, kHot);
            RemoveWindowSubclass(h, dropdown_proc, 2);
            break;
    }
    return DefSubclassProc(h, message, wparam, lparam);
}

// A slider: the mouse wheel scrolls the page it is on instead of moving the slider (the keys still move it).
LRESULT CALLBACK wheel_to_parent_proc(HWND h, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
    if (message == WM_MOUSEWHEEL) return SendMessageW(GetParent(h), message, wparam, lparam);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(h, wheel_to_parent_proc, 3);
    return DefSubclassProc(h, message, wparam, lparam);
}

}  // namespace

int dropdown_chrome(HFONT font) {
    const int line = font_line(font);
    return line / 2 + line * 6 / 5 + 8;  // the words' margin, the chevron's room, the edges
}

void fit_dropdown(HWND combo) {
    if (!is_class(combo, WC_COMBOBOXW)) return;
    const int line = font_line(combo_font(combo));
    const LONG style = GetWindowLongW(combo, GWL_STYLE);
    if ((style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) != 0) {
        // The closed box about a line and a half high (two at the edges), the rows of the open list a little more.
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), line * 3 / 2);
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, line * 9 / 5);
    }
    SetWindowSubclass(combo, dropdown_proc, 2, 0);
    if (g_palette.custom) place_edit(combo);
    COMBOBOXINFO info{};
    info.cbSize = sizeof(info);
    if (windows_build() >= 22000 && GetComboBoxInfo(combo, &info) && info.hwndList != nullptr) {
        const DWORD round = 3 /*DWMWCP_ROUNDSMALL*/;
        DwmSetWindowAttribute(info.hwndList, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof(round));
    }
    InvalidateRect(combo, nullptr, TRUE);
}

bool measure_item(MEASUREITEMSTRUCT* measure) {
    if (measure->CtlType != ODT_COMBOBOX) return false;
    // fit_dropdown() sets the real heights once the box has its font.
    measure->itemHeight = static_cast<UINT>(font_line(static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))) * 9 / 5);
    return true;
}

bool draw_item(const DRAWITEMSTRUCT* draw) {
    if (draw->CtlType != ODT_COMBOBOX) return false;
    const Palette& p = g_palette;
    HWND h = draw->hwndItem;
    HDC dc = draw->hDC;
    const RECT& r = draw->rcItem;
    const bool in_box = (draw->itemState & ODS_COMBOBOXEDIT) != 0;  // the closed box's words (high contrast)
    const bool selected = (draw->itemState & ODS_SELECTED) != 0;
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0 || !IsWindowEnabled(h);
    HFONT font = combo_font(h);
    HGDIOBJ old_font = SelectObject(dc, font);
    const int line = font_line(font);
    const std::wstring text = combo_item_text(h, static_cast<int>(draw->itemID));
    RECT t{r.left + line / 2 + 4, r.top, r.right - 4, r.bottom};
    SetBkMode(dc, TRANSPARENT);
    if (!p.custom) {
        // High contrast: the system's colours, as Windows draws a list.
        const bool highlight = selected && !in_box;
        FillRect(dc, &r, GetSysColorBrush(highlight ? COLOR_HIGHLIGHT : COLOR_WINDOW));
        SetTextColor(dc, GetSysColor(disabled ? COLOR_GRAYTEXT : highlight ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if ((draw->itemState & ODS_FOCUS) != 0 && !in_box) DrawFocusRect(dc, &r);
        SelectObject(dc, old_font);
        return true;
    }
    FillRect(dc, &r, in_box ? surface_brush() : field_brush());
    if (selected && !in_box) {
        // The row under the pointer or the keys: a rounded highlight with the accent mark, as in Windows 11's lists.
        const RECT face{r.left + 4, r.top + 2, r.right - 4, r.bottom - 2};
        round_rect(dc, face, std::max(4.0f, line / 4.0f), p.selected, p.selected, 0.0f);
        const int mark_h = (r.bottom - r.top) * 2 / 5;
        const RECT mark{face.left, r.top + (r.bottom - r.top - mark_h) / 2, face.left + 3, r.top + (r.bottom - r.top - mark_h) / 2 + mark_h};
        round_rect(dc, mark, 1.5f, p.accent, p.accent, 0.0f);
    }
    SetTextColor(dc, disabled ? p.muted : p.field_text);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old_font);
    return true;
}

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
    return theme == Theme::Dark || theme == Theme::Shadow || (theme == Theme::System && windows_prefers_dark());
}

void set_dark(bool dark, bool shadow) {
    if (g_owned) {
        DeleteObject(g_window);
        DeleteObject(g_surface);
        DeleteObject(g_field);
    }
    Palette& p = g_palette;
    p = Palette{};
    p.dark = dark;
    p.custom = !high_contrast();
    if (!p.custom) {
        // High contrast: the system's colours, Windows' own controls.
        p.window = p.surface = GetSysColor(COLOR_BTNFACE);
        p.surface_stroke = p.border = p.control_stroke = GetSysColor(COLOR_WINDOWTEXT);
        p.text = GetSysColor(COLOR_BTNTEXT);
        p.muted = GetSysColor(COLOR_GRAYTEXT);
        p.field = GetSysColor(COLOR_WINDOW);
        p.field_text = GetSysColor(COLOR_WINDOWTEXT);
        p.control = p.control_hot = p.control_pressed = GetSysColor(COLOR_BTNFACE);
        p.selected = p.accent = GetSysColor(COLOR_HIGHLIGHT);
        p.accent_text = GetSysColor(COLOR_HIGHLIGHTTEXT);
        g_window = g_surface = GetSysColorBrush(COLOR_BTNFACE);
        g_field = GetSysColorBrush(COLOR_WINDOW);
        g_owned = false;
    } else if (dark) {
        // Windows 11's dark colours (the WinUI theme resources, made opaque on the dark window).
        p.window = RGB(0x20, 0x20, 0x20);
        p.surface = RGB(0x2B, 0x2B, 0x2B);
        p.surface_stroke = RGB(0x1C, 0x1C, 0x1C);
        p.text = RGB(0xFF, 0xFF, 0xFF);
        p.muted = RGB(0xC5, 0xC5, 0xC5);
        p.field = RGB(0x2D, 0x2D, 0x2D);
        p.field_text = RGB(0xFF, 0xFF, 0xFF);
        p.border = RGB(0x9E, 0x9E, 0x9E);
        p.control = RGB(0x37, 0x37, 0x37);
        p.control_hot = RGB(0x3D, 0x3D, 0x3D);
        p.control_pressed = RGB(0x32, 0x32, 0x32);
        p.control_stroke = RGB(0x45, 0x45, 0x45);
        p.selected = RGB(0x2D, 0x2D, 0x2D);
        g_owned = true;
    } else {
        // Windows 11's light colours.
        p.window = RGB(0xF3, 0xF3, 0xF3);
        p.surface = RGB(0xFB, 0xFB, 0xFB);
        p.surface_stroke = RGB(0xE5, 0xE5, 0xE5);
        p.text = RGB(0x1B, 0x1B, 0x1B);
        p.muted = RGB(0x5D, 0x5D, 0x5D);
        p.field = RGB(0xFF, 0xFF, 0xFF);
        p.field_text = RGB(0x1B, 0x1B, 0x1B);
        p.border = RGB(0x86, 0x86, 0x86);
        p.control = RGB(0xFE, 0xFE, 0xFE);
        p.control_hot = RGB(0xF6, 0xF6, 0xF6);
        p.control_pressed = RGB(0xEE, 0xEE, 0xEE);
        p.control_stroke = RGB(0xD0, 0xD0, 0xD0);
        p.selected = RGB(0xE9, 0xE9, 0xE9);
        g_owned = true;
    }
    if (p.custom && dark && shadow) {
        // The Shadow theme: near-black surfaces with a warm tint, and Shadow's red as the accent.
        p.window = RGB(0x12, 0x11, 0x12);
        p.surface = RGB(0x1F, 0x1C, 0x1D);
        p.surface_stroke = RGB(0x0A, 0x09, 0x09);
        p.muted = RGB(0xC8, 0xBE, 0xBE);
        p.field = RGB(0x24, 0x20, 0x21);
        p.border = RGB(0xA0, 0x92, 0x92);
        p.control = RGB(0x2C, 0x27, 0x28);
        p.control_hot = RGB(0x35, 0x2E, 0x2F);
        p.control_pressed = RGB(0x26, 0x22, 0x23);
        p.control_stroke = RGB(0x46, 0x3A, 0x3B);
        p.selected = RGB(0x2E, 0x22, 0x24);
    }
    if (p.custom) {
        p.accent = dark && shadow ? RGB(0xE0, 0x2F, 0x3A) : windows_accent(dark);
        p.accent_text = luminance(p.accent) > 0.55 ? RGB(0, 0, 0) : RGB(0xFF, 0xFF, 0xFF);
        g_window = CreateSolidBrush(p.window);
        g_surface = CreateSolidBrush(p.surface);
        g_field = CreateSolidBrush(p.field);
    }
    const DarkApi& api = dark_api();
    if (api.set_preferred_app_mode != nullptr) api.set_preferred_app_mode(dark ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
    if (api.flush_menu_themes != nullptr) api.flush_menu_themes();
}

const Palette& colors() { return g_palette; }

bool colors_differ(bool dark, bool shadow) {
    const Palette& p = g_palette;
    if (p.dark != dark || p.custom == high_contrast()) return true;
    if (!p.custom) return p.window != GetSysColor(COLOR_BTNFACE) || p.text != GetSysColor(COLOR_BTNTEXT);
    return !(dark && shadow) && p.accent != windows_accent(dark);
}
HBRUSH window_brush() { return g_window != nullptr ? g_window : GetSysColorBrush(COLOR_BTNFACE); }
HBRUSH surface_brush() { return g_surface != nullptr ? g_surface : window_brush(); }
HBRUSH field_brush() { return g_field != nullptr ? g_field : GetSysColorBrush(COLOR_WINDOW); }

const wchar_t* text_face() {
    static const wchar_t* const face = font_installed(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    return face;
}

const wchar_t* title_face() {
    static const wchar_t* const face =
        font_installed(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI Semibold";
    return face;
}

const wchar_t* icon_face() {
    static const wchar_t* const face = font_installed(L"Segoe Fluent Icons")   ? L"Segoe Fluent Icons"
                                       : font_installed(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets"
                                                                              : L"";
    return face;
}

void apply_to_window(HWND window) {
    const bool dark = g_palette.dark;
    const BOOL value = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(window, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &value, sizeof(value));
    if (windows_build() >= 22000) {
        // Windows 11: rounded corners, and a title bar in the window's own colour (one surface from top to bottom).
        const DWORD round = 2 /*DWMWCP_ROUND*/;
        DwmSetWindowAttribute(window, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof(round));
        const COLORREF caption = g_palette.custom ? g_palette.window : 0xFFFFFFFF /*DWMWA_COLOR_DEFAULT*/;
        DwmSetWindowAttribute(window, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
        const COLORREF caption_text = g_palette.custom ? g_palette.text : 0xFFFFFFFF;
        DwmSetWindowAttribute(window, 36 /*DWMWA_TEXT_COLOR*/, &caption_text, sizeof(caption_text));
    }
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
        fit_dropdown(h);
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
    } else if (is_class(h, WC_LISTBOXW)) {
        set_theme(h, dark, L"DarkMode_Explorer");
    } else if (is_class(h, TRACKBAR_CLASSW)) {
        SetWindowSubclass(h, wheel_to_parent_proc, 3, 0);
        // TBS_FIXEDLENGTH: the knob as large as the zoom makes the slider, centred on its height.
        if ((GetWindowLongW(h, GWL_STYLE) & TBS_FIXEDLENGTH) != 0) {
            RECT r;
            GetClientRect(h, &r);
            SendMessageW(h, TBM_SETTHUMBLENGTH, static_cast<WPARAM>(std::max(12L, r.bottom * 2 / 3)), 0);
        }
    } else if ((GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_CONTROLPARENT) != 0) {
        set_theme(h, dark, L"DarkMode_Explorer");  // a program's own panel of controls: its scroll bar
    }
    InvalidateRect(h, nullptr, TRUE);
}

void set_primary(HWND button) { SetPropW(button, kPrimary, reinterpret_cast<HANDLE>(1)); }
void set_switch(HWND check_box) { SetPropW(check_box, kSwitch, reinterpret_cast<HANDLE>(1)); }
void set_icon(HWND button, wchar_t glyph) { SetPropW(button, kIcon, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(glyph))); }

LRESULT control_color(UINT message, HDC dc, HWND control, bool muted, bool on_surface) {
    const Palette& p = g_palette;
    const bool field = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX ||
                       (p.custom && message == WM_CTLCOLORSTATIC && is_class(control, WC_EDITW));  // read-only text
    if (field) {
        SetTextColor(dc, p.field_text);
        SetBkColor(dc, p.field);
        return reinterpret_cast<LRESULT>(field_brush());
    }
    SetTextColor(dc, muted ? p.muted : p.text);
    SetBkColor(dc, on_surface ? p.surface : p.window);
    return reinterpret_cast<LRESULT>(on_surface ? surface_brush() : window_brush());
}

bool custom_draw(const NMHDR* header, LRESULT* result) {
    if (!g_palette.custom || header->code != NM_CUSTOMDRAW) return false;
    const auto& cd = *reinterpret_cast<const NMCUSTOMDRAW*>(header);
    if (is_class(header->hwndFrom, WC_BUTTONW)) {
        const LONG type = GetWindowLongW(header->hwndFrom, GWL_STYLE) & BS_TYPEMASK;
        if (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX) {
            *result = draw_check_box(cd);
            return true;
        }
        if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) {
            *result = draw_button(cd);
            return true;
        }
        return false;
    }
    if (is_class(header->hwndFrom, TRACKBAR_CLASSW)) {
        *result = draw_slider(cd);
        return true;
    }
    return false;
}

void paint_card(HDC dc, const RECT& r, int radius) {
    round_rect(dc, r, static_cast<float>(radius), g_palette.surface, g_palette.surface_stroke);
}

void paint_nav_item(HDC dc, const RECT& r, const wchar_t* text, wchar_t glyph, bool chosen, bool focused, HFONT font,
                    HFONT icons) {
    const Palette& p = g_palette;
    FillRect(dc, &r, window_brush());
    const int h = r.bottom - r.top;
    RECT face{r.left + 4, r.top + 2, r.right - 4, r.bottom - 2};
    if (chosen) {
        round_rect(dc, face, 5.0f, p.selected, p.selected, 0.0f);
        // The accent mark at the left, as in Windows' own Settings.
        const int mark_h = h * 2 / 5;
        RECT mark{face.left, r.top + (h - mark_h) / 2, face.left + 3, r.top + (h - mark_h) / 2 + mark_h};
        round_rect(dc, mark, 1.5f, p.accent, p.accent, 0.0f);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, p.text);
    if (text == nullptr || *text == L'\0') {  // a narrow list: the icon alone, centred
        if (glyph != 0 && icons != nullptr) {
            HGDIOBJ previous = SelectObject(dc, icons);
            RECT icon_box = face;
            icon_box.left += 2;  // clear of the accent mark
            DrawTextW(dc, &glyph, 1, &icon_box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, previous);
        }
        if (focused) focus_ring(dc, face, 5.0f);
        return;
    }
    int x = face.left + h / 2;
    if (glyph != 0 && icons != nullptr) {
        HGDIOBJ previous = SelectObject(dc, icons);
        RECT icon_box{x - 2, r.top, x + h * 2 / 3, r.bottom};
        DrawTextW(dc, &glyph, 1, &icon_box, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, previous);
        x += h * 3 / 4 + 4;
    }
    HGDIOBJ previous = SelectObject(dc, font);
    RECT t{x, r.top, face.right - 4, r.bottom};
    DrawTextW(dc, text, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, previous);
    if (focused) focus_ring(dc, face, 5.0f);
}

int font_height(int points, int dpi, int text_size) { return -MulDiv(points * text_size, dpi, 72 * 100); }

void set_icon_only(HWND button, bool icon_only) {
    const bool now = GetPropW(button, kIconOnly) != nullptr;
    if (now == icon_only) return;
    if (icon_only) SetPropW(button, kIconOnly, reinterpret_cast<HANDLE>(1));
    else RemovePropW(button, kIconOnly);
    InvalidateRect(button, nullptr, TRUE);
}

bool can_show_icon_only(HWND button) {
    return g_palette.custom && GetPropW(button, kIcon) != nullptr && *icon_face() != L'\0';
}

SIZE text_extent(HFONT font, const std::wstring& text, int wrap) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, font);
    RECT r{0, 0, wrap > 0 ? wrap : 0, 0};
    if (text.empty()) {
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        r.bottom = tm.tmHeight;
        r.right = 0;
    } else {
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r,
                  DT_CALCRECT | DT_NOPREFIX | DT_EXPANDTABS | (wrap > 0 ? DT_WORDBREAK : DT_SINGLELINE));
    }
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return SIZE{r.right - r.left, r.bottom - r.top};
}

namespace {
HFONT font_of(HWND h) {
    auto font = reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0));
    return font != nullptr ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}
int line_height(HFONT font) { return text_extent(font, L"", 0).cy; }
}  // namespace

int button_width(HWND button) { return button_width(button, GetPropW(button, kIconOnly) != nullptr); }

int button_width(HWND button, bool icon_only) {
    HFONT font = font_of(button);
    const int line = line_height(font);
    const bool icon = g_palette.custom && GetPropW(button, kIcon) != nullptr && *icon_face() != L'\0';
    if (icon && icon_only) return line * 2 + 4;
    const int words = text_extent(font, text_of(button), 0).cx;
    return words + (icon ? line + line / 2 : 0) + line * 3 / 2 + 4;
}

int check_box_width(HWND check_box) {
    HFONT font = font_of(check_box);
    const int line = line_height(font);
    // The switch is two lines wide and a half line from its words; a box is about a line with half a line after it.
    const int mark = g_palette.custom ? (GetPropW(check_box, kSwitch) != nullptr ? line * 2 + 1 + line / 2 : line * 4 / 5 + 1 + line * 2 / 5)
                                      : GetSystemMetrics(SM_CXMENUCHECK) + 6;
    return mark + text_extent(font, text_of(check_box), 0).cx + 4;
}

int check_box_height(HWND check_box, int width) {
    HFONT font = font_of(check_box);
    const int words = width - (check_box_width(check_box) - text_extent(font, text_of(check_box), 0).cx);
    return text_extent(font, text_of(check_box), std::max(1, words)).cy + 4;
}

std::vector<std::wstring> find_clipped(HWND parent, bool whole_height) {
    std::vector<std::wstring> problems;
    RECT client;
    GetClientRect(parent, &client);
    for (HWND c = GetWindow(parent, GW_CHILD); c != nullptr; c = GetWindow(c, GW_HWNDNEXT)) {
        if ((GetWindowLongW(c, GWL_STYLE) & WS_VISIBLE) == 0) continue;  // its own flag: the window may be hidden
        if ((GetWindowLongW(c, GWL_EXSTYLE) & WS_EX_CONTROLPARENT) != 0) {
            // A panel of controls that scrolls: its controls must fit its width; it scrolls for their height.
            for (const std::wstring& inner : find_clipped(c, false)) problems.push_back(L"(in a panel) " + inner);
        }
        RECT r;
        GetWindowRect(c, &r);
        MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&r), 2);
        const int w = r.right - r.left;
        int h = r.bottom - r.top;
        const std::wstring text = text_of(c);
        const LONG style = GetWindowLongW(c, GWL_STYLE);
        HFONT font = font_of(c);
        std::wstring why;
        const auto need = [&](const wchar_t* what, int needed, int had) {
            if (needed > had && why.empty())
                why = std::wstring(what) + L" needs " + std::to_wstring(needed) + L" px, has " + std::to_wstring(had);
        };
        if (is_class(c, WC_STATICW)) {
            const LONG kind = style & SS_TYPEMASK;
            if (text.empty() || kind == SS_OWNERDRAW || kind == SS_ICON || kind == SS_BITMAP) continue;
            const bool one_line = kind == SS_LEFTNOWORDWRAP || (style & (SS_CENTERIMAGE | SS_ENDELLIPSIS)) != 0;
            if (one_line) {
                const SIZE s = text_extent(font, text, 0);
                need(L"width", s.cx, w);
                need(L"height", s.cy, h);
            } else {
                const SIZE s = text_extent(font, text, w);
                need(L"width (a word)", s.cx, w);
                need(L"height", s.cy, h);
            }
        } else if (is_class(c, WC_BUTTONW)) {
            const LONG type = style & BS_TYPEMASK;
            if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) {
                need(L"width", button_width(c), w);
                need(L"height", line_height(font) + 4, h);
            } else if (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX) {
                if ((style & BS_MULTILINE) != 0) need(L"height", check_box_height(c, w), h);
                else need(L"width", check_box_width(c), w);
            }
        } else if (is_class(c, WC_COMBOBOXW)) {
            COMBOBOXINFO info{};
            info.cbSize = sizeof(info);
            const int sel = static_cast<int>(SendMessageW(c, CB_GETCURSEL, 0, 0));
            if (sel >= 0 && GetComboBoxInfo(c, &info)) {
                const std::wstring item = combo_item_text(c, sel);
                if (g_palette.custom && !editable_combo(c))  // drawn here: its words, their margin and the chevron
                    need(L"width of the choice", text_extent(font, item, 0).cx + dropdown_chrome(font), w);
                else
                    need(L"width of the choice", text_extent(font, item, 0).cx + 6, info.rcItem.right - info.rcItem.left);
            }
            h = 0;  // a combo box's height includes its list
        }
        if (why.empty() && (r.left < client.left || r.right > client.right))
            why = L"crosses the window's side (" + std::to_wstring(r.left) + L".." + std::to_wstring(r.right) + L" of " +
                  std::to_wstring(client.right) + L" px)";
        if (why.empty() && whole_height && h > 0 && (r.top < client.top || r.bottom > client.bottom))
            why = L"crosses the window's bottom (" + std::to_wstring(r.top) + L".." + std::to_wstring(r.bottom) + L" of " +
                  std::to_wstring(client.bottom) + L" px)";
        if (why.empty() && w > 0 && h > 0 && (w < 4 || h < 4)) why = L"is almost no size";
        if (!why.empty()) {
            wchar_t cls[64] = {};
            GetClassNameW(c, cls, static_cast<int>(std::size(cls)));
            problems.push_back(std::wstring(cls) + L" #" + std::to_wstring(GetDlgCtrlID(c)) + L" \"" + text.substr(0, 60) +
                               L"\": " + why);
        }
    }
    return problems;
}

void start_drawing() {
    if (g_gdiplus != 0) return;
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_gdiplus, &input, nullptr);
}

void stop_drawing() {
    if (g_gdiplus == 0) return;
    Gdiplus::GdiplusShutdown(g_gdiplus);
    g_gdiplus = 0;
}

}  // namespace ui
