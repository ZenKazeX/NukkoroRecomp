#define NOMINMAX
#include "controller_view.h"

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

#include <cstring>
#include <map>

#include "art/hotspots.inc"

namespace padconfig {

namespace sp = shadow::peripheral;
using namespace Gdiplus;

namespace {

// Everything is laid out in these units and scaled to the window.
constexpr REAL kViewW = 660.0f, kViewH = 590.0f;
constexpr REAL kPictureX = 130.0f, kPictureW = 400.0f;
constexpr REAL kTopY = 8.0f, kTiltedY = 306.0f;
constexpr REAL kColumnW = 122.0f;
const Color kInk(255, 28, 28, 28), kLine(255, 110, 110, 110);

std::wstring wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

int control_of(const char* key) {
    for (std::size_t i = 0; i < kControls.size(); ++i)
        if (std::strcmp(kControls[i].key, key) == 0) return static_cast<int>(i);
    return -1;
}

Bitmap* load_art(int id) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (found == nullptr) return nullptr;
    const DWORD size = SizeofResource(module, found);
    const void* data = LockResource(LoadResource(module, found));
    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
    if (copy == nullptr || data == nullptr) return nullptr;
    std::memcpy(GlobalLock(copy), data, size);
    GlobalUnlock(copy);
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(copy, TRUE, &stream))) return nullptr;
    Bitmap* bitmap = Bitmap::FromStream(stream);
    stream->Release();
    return bitmap;
}

Bitmap* art(int id) {
    // Kept for the program's life: they must not be destroyed after GDI+ is shut down.
    static Bitmap* const top = load_art(101);
    static Bitmap* const tilted = load_art(102);
    return id == 101 ? top : tilted;
}

// A button's mask from the render: its shape in red, an outline along its edge in green.
struct Marks {
    Bitmap* raw = nullptr;     // the mask itself, for clicks
    Bitmap* pressed = nullptr;
    Bitmap* picked = nullptr;
};

// The mask in one colour, the shape lightly filled and the outline solid, premultiplied for GDI+.
Bitmap* tinted(Bitmap* raw, BYTE red, BYTE green, BYTE blue, double fill) {
    const UINT w = raw->GetWidth(), h = raw->GetHeight();
    auto* out = new Bitmap(static_cast<INT>(w), static_cast<INT>(h), PixelFormat32bppPARGB);
    Rect all(0, 0, static_cast<INT>(w), static_cast<INT>(h));
    BitmapData src{}, dst{};
    raw->LockBits(&all, ImageLockModeRead, PixelFormat32bppARGB, &src);
    out->LockBits(&all, ImageLockModeWrite, PixelFormat32bppPARGB, &dst);
    for (UINT y = 0; y < h; ++y) {
        const auto* in = reinterpret_cast<const BYTE*>(src.Scan0) + y * static_cast<UINT>(src.Stride);
        auto* to = reinterpret_cast<BYTE*>(dst.Scan0) + y * static_cast<UINT>(dst.Stride);
        for (UINT x = 0; x < w; ++x, in += 4, to += 4) {  // B G R A
            const double a = std::max(in[2] / 255.0 * fill, in[1] / 255.0);
            to[0] = static_cast<BYTE>(blue * a + 0.5);
            to[1] = static_cast<BYTE>(green * a + 0.5);
            to[2] = static_cast<BYTE>(red * a + 0.5);
            to[3] = static_cast<BYTE>(255.0 * a + 0.5);
        }
    }
    out->UnlockBits(&dst);
    raw->UnlockBits(&src);
    return out;
}

// Kept for the program's life, like the pictures.
const Marks& marks(int id) {
    static std::map<int, Marks> cache;
    auto it = cache.find(id);
    if (it != cache.end()) return it->second;
    Marks m;
    m.raw = load_art(id);
    if (m.raw != nullptr) {
        m.pressed = tinted(m.raw, 235, 45, 35, 0.42);
        m.picked = tinted(m.raw, 255, 190, 30, 0.34);
    }
    return cache.emplace(id, m).first->second;
}

RectF top_rect() { return RectF(kPictureX, kTopY, kPictureW, kPictureW / kTopAspect); }
RectF tilted_rect() { return RectF(kPictureX, kTiltedY, kPictureW, kPictureW / kTiltedAspect); }
PointF at(const RectF& r, REAL x, REAL y) { return PointF(r.X + r.Width * x, r.Y + r.Height * y); }

template <std::size_t N>
const Hotspot* find(const Hotspot (&list)[N], const char* key) {
    for (const Hotspot& h : list)
        if (std::strcmp(h.key, key) == 0) return &h;
    return nullptr;
}

struct Painter {
    Graphics& g;
    const ControllerView& v;
    FontFamily family{L"Segoe UI"};
    Font label{&family, 14.5f, FontStyleBold, UnitPixel};
    Font caption{&family, 12.5f, FontStyleRegular, UnitPixel};

    void text(const std::wstring& s, const Font& f, REAL x, REAL y, REAL w, StringAlignment align, const Color& c) {
        StringFormat fmt;
        fmt.SetAlignment(align);
        fmt.SetLineAlignment(StringAlignmentCenter);
        SolidBrush b(c);
        g.DrawString(s.c_str(), -1, &f, RectF(x, y - 12, w, 24), &fmt, &b);
    }

    void leader(PointF from, PointF to) {
        Pen p(kLine, 1.1f);
        g.DrawLine(&p, from, to);
        SolidBrush dot(kLine);
        g.FillEllipse(&dot, to.X - 2.2f, to.Y - 2.2f, 4.4f, 4.4f);
    }

    void left(REAL y, const std::wstring& s, PointF to) {
        text(s, label, 0, y, kColumnW, StringAlignmentFar, kInk);
        leader(PointF(kColumnW + 3, y), to);
    }
    void right(REAL y, const std::wstring& s, PointF to) {
        text(s, label, kViewW - kColumnW, y, kColumnW, StringAlignmentNear, kInk);
        leader(PointF(kViewW - kColumnW - 3, y), to);
    }

    bool pressed(int control) const {
        const auto& c = kControls[static_cast<std::size_t>(control)];
        if ((v.held & (v.binding.*c.field)) != 0u) return true;
        return (c.field == &GamepadBinding::l && v.left_trigger) || (c.field == &GamepadBinding::r && v.right_trigger);
    }

    void mark(const RectF& r, const Hotspot& h) {
        const int control = control_of(h.key);
        if (control < 0) return;
        const bool lit = pressed(control), picked = control == v.selected;
        if (!lit && !picked) return;
        const Marks& m = marks(h.id);
        if (Bitmap* b = lit ? m.pressed : m.picked)
            g.DrawImage(b, RectF(r.X + r.Width * h.x, r.Y + r.Height * h.y, r.Width * h.w, r.Height * h.h));
    }

    std::wstring of(const char* key) { return control_label(v.binding, static_cast<std::size_t>(control_of(key))); }
    PointF top(const char* key) {
        const Hotspot* h = find(kTopHotspots, key);
        return at(top_rect(), h->cx, h->cy);
    }
    PointF tilted(const char* key) {
        const Hotspot* h = find(kTiltedHotspots, key);
        return at(tilted_rect(), h->cx, h->cy);
    }

    void draw_pictures() {
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        if (Bitmap* b = art(101)) g.DrawImage(b, top_rect());
        if (Bitmap* b = art(102)) g.DrawImage(b, tilted_rect());
        for (const Hotspot& h : kTopHotspots) mark(top_rect(), h);
        for (const Hotspot& h : kTiltedHotspots) mark(tilted_rect(), h);
        g.SetPixelOffsetMode(PixelOffsetModeDefault);
    }

    void draw_labels() {
        const std::wstring main_stick = v.binding.swap_sticks ? L"Right stick" : L"Left stick";
        const std::wstring c_stick = v.binding.swap_sticks ? L"Left stick" : L"Right stick";
        left(40, main_stick, top("stick"));
        left(118, of("dpad_up"), top("dpad_up"));
        left(154, of("dpad_left"), top("dpad_left"));
        left(190, of("dpad_down"), top("dpad_down"));
        left(226, of("dpad_right"), top("dpad_right"));
        right(34, of("y"), top("y"));
        right(76, of("x"), top("x"));
        right(118, of("a"), top("a"));
        right(160, of("b"), top("b"));
        right(226, c_stick, top("cstick"));
        const PointF start = top("start");
        text(of("start"), label, start.X - 60, start.Y + 62, 120, StringAlignmentCenter, kInk);
        leader(PointF(start.X, start.Y + 50), PointF(start.X, start.Y + 7));
        text(L"Click a button on the picture to change it", caption, kPictureX - 40, kTiltedY - 18, kPictureW + 80, StringAlignmentCenter, kLine);

        left(kTiltedY + 44, of("l"), tilted("l"));
        right(kTiltedY + 26, of("r"), tilted("r"));
        right(kTiltedY + 92, of("z"), tilted("z"));
    }
};

template <std::size_t N>
int hit(const Hotspot (&list)[N], const RectF& r, PointF p) {
    for (const Hotspot& h : list) {
        const int control = control_of(h.key);
        if (control < 0) continue;
        Bitmap* raw = marks(h.id).raw;
        if (raw == nullptr || h.w <= 0.0f) continue;
        // On the button's real shape, with a few units' grace so small buttons are easy to hit.
        const REAL unit_x = static_cast<REAL>(raw->GetWidth()) / (r.Width * h.w), unit_y = static_cast<REAL>(raw->GetHeight()) / (r.Height * h.h);
        const REAL u = (p.X - (r.X + r.Width * h.x)) * unit_x, v = (p.Y - (r.Y + r.Height * h.y)) * unit_y;
        constexpr REAL kGrace = 3.0f;
        for (const PointF offset : {PointF(0, 0), PointF(-kGrace, 0), PointF(kGrace, 0), PointF(0, -kGrace), PointF(0, kGrace)}) {
            const INT x = static_cast<INT>(u + offset.X * unit_x), y = static_cast<INT>(v + offset.Y * unit_y);
            if (x < 0 || y < 0 || x >= static_cast<INT>(raw->GetWidth()) || y >= static_cast<INT>(raw->GetHeight())) continue;
            Color c;
            raw->GetPixel(x, y, &c);
            if (c.GetR() > 128) return control;
        }
    }
    return -1;
}

REAL scale_of(const RECT& area) {
    return std::min(static_cast<REAL>(area.right - area.left) / kViewW, static_cast<REAL>(area.bottom - area.top) / kViewH);
}

}  // namespace

bool operator==(const ControllerView& a, const ControllerView& b) noexcept {
    return same(a.binding, b.binding) && a.held == b.held && a.left_trigger == b.left_trigger && a.right_trigger == b.right_trigger &&
           a.selected == b.selected;
}

std::wstring control_label(const GamepadBinding& binding, std::size_t index) {
    const std::uint16_t mask = binding.*kControls[index].field;
    const bool is_l = kControls[index].field == &GamepadBinding::l, is_r = kControls[index].field == &GamepadBinding::r;
    if (!is_l && !is_r) return wide(button_names(mask));
    const std::wstring trigger = is_l ? L"LT" : L"RT";
    return mask == 0u ? trigger : wide(button_names(mask)) + L" or " + trigger;
}

void paint_controller(HDC dc, const RECT& area, const ControllerView& view) {
    if (area.right <= area.left || area.bottom <= area.top) return;
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    const REAL scale = scale_of(area);
    g.TranslateTransform(static_cast<REAL>(area.left), static_cast<REAL>(area.top));
    g.ScaleTransform(scale, scale);
    Painter painter{g, view};
    painter.draw_pictures();
    painter.draw_labels();
}

int control_at(const RECT& area, POINT point) {
    const REAL scale = scale_of(area);
    const PointF p(static_cast<REAL>(point.x - area.left) / scale, static_cast<REAL>(point.y - area.top) / scale);
    if (const int c = hit(kTopHotspots, top_rect(), p); c >= 0) return c;
    return hit(kTiltedHotspots, tilted_rect(), p);
}

}  // namespace padconfig
