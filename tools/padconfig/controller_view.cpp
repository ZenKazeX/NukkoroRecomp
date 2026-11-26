#define NOMINMAX
#include "controller_view.h"

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

#include <cmath>
#include <cstring>
#include <string>
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
// The labels' ink and the leader lines, on the light and on the dark background.
const Color kInk(255, 28, 28, 28), kLine(255, 110, 110, 110);
const Color kDarkInk(255, 242, 242, 242), kDarkLine(255, 165, 165, 165);

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

// ---- the sticks: their caps, cut out of the picture from above, drawn tilted the way the player holds them ------------

// A stick's cap in the picture from above (fractions of the picture's width: its radius, and the radius of the dark
// gate around it), measured on the render.
struct StickArt {
    const char* key;
    float cap, gate;
};
constexpr StickArt kSticks[] = {{"stick", 0.076f, 0.088f}, {"cstick", 0.071f, 0.084f}};
// How far a cap moves at full tilt, as a fraction of the picture's width (about what the real stick shows from above).
constexpr float kMostTilt = 0.032f;

struct Cap {
    Bitmap* image = nullptr;  // the cap, round, with a soft edge
    Color well;               // the gate's colour, shown where the cap has moved away
};

// Kept for the program's life, like the pictures.
const Cap& cap_of(const StickArt& stick) {
    static std::map<std::string, Cap> cache;
    auto it = cache.find(stick.key);
    if (it != cache.end()) return it->second;
    Cap c;
    Bitmap* top = art(101);
    const Hotspot* h = nullptr;
    for (const Hotspot& spot : kTopHotspots)
        if (std::strcmp(spot.key, stick.key) == 0) h = &spot;
    if (top != nullptr && h != nullptr) {
        const INT w = static_cast<INT>(top->GetWidth()), ht = static_cast<INT>(top->GetHeight());
        const REAL cx = w * h->cx, cy = ht * h->cy, r = w * stick.cap;
        const INT size = static_cast<INT>(std::ceil(r * 2.0f)) + 2;
        auto* image = new Bitmap(size, size, PixelFormat32bppARGB);
        {
            Graphics g(image);
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            g.DrawImage(top, RectF(0, 0, static_cast<REAL>(size), static_cast<REAL>(size)), cx - size / 2.0f, cy - size / 2.0f,
                        static_cast<REAL>(size), static_cast<REAL>(size), UnitPixel);
        }
        Rect all(0, 0, size, size);
        BitmapData data{};
        image->LockBits(&all, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppARGB, &data);
        for (INT y = 0; y < size; ++y) {
            auto* row = reinterpret_cast<BYTE*>(data.Scan0) + y * data.Stride;
            for (INT x = 0; x < size; ++x) {
                const REAL dx = x + 0.5f - size / 2.0f, dy = y + 0.5f - size / 2.0f;
                const REAL inside = std::clamp(r - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);  // a soft edge
                row[x * 4 + 3] = static_cast<BYTE>(row[x * 4 + 3] * inside);
            }
        }
        image->UnlockBits(&data);
        c.image = image;
        // The gate's colour: the picture's average on a ring between the cap and the gate's edge.
        int red = 0, green = 0, blue = 0, n = 0;
        const REAL ring = w * (stick.cap + stick.gate) / 2.0f;
        for (int i = 0; i < 32; ++i) {
            const REAL a = i * 6.2831853f / 32.0f;
            const INT x = static_cast<INT>(cx + ring * std::cos(a)), y = static_cast<INT>(cy + ring * std::sin(a));
            if (x < 0 || y < 0 || x >= w || y >= ht) continue;
            Color px;
            top->GetPixel(x, y, &px);
            red += px.GetR();
            green += px.GetG();
            blue += px.GetB();
            ++n;
        }
        if (n > 0) c.well = Color(255, static_cast<BYTE>(red / n), static_cast<BYTE>(green / n), static_cast<BYTE>(blue / n));
    }
    return cache.emplace(stick.key, c).first->second;
}

// Where things are, in view units: the label columns `f` times their normal width (and the labels f times their normal
// size) on both sides of the pictures, the whole scaled by `scale` to the area and centred across it.
struct Geometry {
    REAL f = 1.0f;
    REAL column = kColumnW;
    REAL picture_x = kPictureX;
    REAL view_w = kViewW;
    REAL scale = 1.0f;
    REAL offset_x = 0.0f;  // pixels
};
constexpr REAL kLabelSize = 14.5f, kCaptionSize = 12.5f;
constexpr REAL kMostLabelGrowth = 1.8f;  // beyond this the stacked D-pad labels would touch

Geometry geometry_of(const RECT& area, REAL min_label) {
    const REAL aw = static_cast<REAL>(area.right - area.left), ah = static_cast<REAL>(area.bottom - area.top);
    Geometry m;
    for (int pass = 0; pass < 8; ++pass) {
        m.column = kColumnW * m.f;
        m.picture_x = m.column + (kPictureX - kColumnW);
        m.view_w = 2.0f * m.picture_x + kPictureW;
        m.scale = std::min(aw / m.view_w, ah / kViewH);
        if (min_label <= 0.0f || kLabelSize * m.f * m.scale >= min_label * 0.98f || m.f >= kMostLabelGrowth) break;
        m.f = std::min(kMostLabelGrowth, min_label / (kLabelSize * m.scale));
    }
    m.offset_x = std::max(0.0f, (aw - m.view_w * m.scale) / 2.0f);
    return m;
}

RectF top_rect(const Geometry& m) { return RectF(m.picture_x, kTopY, kPictureW, kPictureW / kTopAspect); }
RectF tilted_rect(const Geometry& m) { return RectF(m.picture_x, kTiltedY, kPictureW, kPictureW / kTiltedAspect); }
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
    const Geometry& m;
    FontFamily family{L"Segoe UI"};
    Font label{&family, kLabelSize * m.f, FontStyleBold, UnitPixel};
    Font caption{&family, kCaptionSize * m.f, FontStyleRegular, UnitPixel};
    Color ink = kInk, line = kLine;

    void text(const std::wstring& s, const Font& font, REAL x, REAL y, REAL w, StringAlignment align, const Color& c) {
        StringFormat fmt;
        fmt.SetAlignment(align);
        fmt.SetLineAlignment(StringAlignmentCenter);
        SolidBrush b(c);
        g.DrawString(s.c_str(), -1, &font, RectF(x, y - 12 * m.f, w, 24 * m.f), &fmt, &b);
    }

    void leader(PointF from, PointF to) {
        Pen p(line, 1.1f);
        g.DrawLine(&p, from, to);
        SolidBrush dot(line);
        g.FillEllipse(&dot, to.X - 2.2f, to.Y - 2.2f, 4.4f, 4.4f);
    }

    void left(REAL y, const std::wstring& s, PointF to) {
        text(s, label, 0, y, m.column, StringAlignmentFar, ink);
        leader(PointF(m.column + 3, y), to);
    }
    void right(REAL y, const std::wstring& s, PointF to) {
        text(s, label, m.view_w - m.column, y, m.column, StringAlignmentNear, ink);
        leader(PointF(m.view_w - m.column - 3, y), to);
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
        const Marks& tint = marks(h.id);
        if (Bitmap* b = lit ? tint.pressed : tint.picked)
            g.DrawImage(b, RectF(r.X + r.Width * h.x, r.Y + r.Height * h.y, r.Width * h.w, r.Height * h.h));
    }

    std::wstring of(const char* key) { return control_label(v.binding, static_cast<std::size_t>(control_of(key))); }
    PointF top(const char* key) {
        const Hotspot* h = find(kTopHotspots, key);
        return at(top_rect(m), h->cx, h->cy);
    }
    PointF tilted(const char* key) {
        const Hotspot* h = find(kTiltedHotspots, key);
        return at(tilted_rect(m), h->cx, h->cy);
    }

    // A stick held away from the middle: its gate shows where the cap was, and the cap is drawn moved that way.
    void draw_stick(const StickArt& stick, float x, float y) {
        if (x == 0.0f && y == 0.0f) return;
        const Cap& c = cap_of(stick);
        if (c.image == nullptr) return;
        const RectF r = top_rect(m);
        const PointF centre = top(stick.key);
        const REAL radius = r.Width * stick.cap;
        SolidBrush well(c.well);
        g.FillEllipse(&well, centre.X - radius, centre.Y - radius, radius * 2.0f, radius * 2.0f);
        const REAL shift = r.Width * kMostTilt;
        const REAL size = c.image->GetWidth() * r.Width / art(101)->GetWidth();  // its pixels at the picture's scale
        const PointF moved(centre.X + x * shift, centre.Y - y * shift);
        g.DrawImage(c.image, RectF(moved.X - size / 2.0f, moved.Y - size / 2.0f, size, size));
        // Lit like a pressed button: a red ring round the cap, stronger the further it is tilted.
        const REAL amount = std::min(1.0f, std::sqrt(x * x + y * y));
        Pen ring(Color(static_cast<BYTE>(90 + 120 * amount), 235, 45, 35), 3.0f);
        g.DrawEllipse(&ring, moved.X - radius, moved.Y - radius, radius * 2.0f, radius * 2.0f);
    }

    void draw_pictures() {
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        if (Bitmap* b = art(101)) g.DrawImage(b, top_rect(m));
        draw_stick(kSticks[0], v.main_x, v.main_y);
        draw_stick(kSticks[1], v.c_x, v.c_y);
        if (Bitmap* b = art(102)) g.DrawImage(b, tilted_rect(m));
        for (const Hotspot& h : kTopHotspots) mark(top_rect(m), h);
        for (const Hotspot& h : kTiltedHotspots) mark(tilted_rect(m), h);
        g.SetPixelOffsetMode(PixelOffsetModeDefault);
    }

    void draw_labels() {
        const std::wstring main_stick = v.binding.swap_sticks ? wide("Right stick") : wide("Left stick");
        const std::wstring c_stick = v.binding.swap_sticks ? wide("Left stick") : wide("Right stick");
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
        text(of("start"), label, start.X - 60 * m.f, start.Y + 62, 120 * m.f, StringAlignmentCenter, ink);
        leader(PointF(start.X, start.Y + 50), PointF(start.X, start.Y + 7));
        text(wide("Click a button on the picture to change it"), caption, 0, kTiltedY - 18, m.view_w, StringAlignmentCenter, line);

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


}  // namespace

bool operator==(const ControllerView& a, const ControllerView& b) noexcept {
    return same(a.binding, b.binding) && a.held == b.held && a.left_trigger == b.left_trigger && a.right_trigger == b.right_trigger &&
           a.selected == b.selected && a.main_x == b.main_x && a.main_y == b.main_y && a.c_x == b.c_x && a.c_y == b.c_y;
}

std::wstring control_label(const GamepadBinding& binding, std::size_t index) {
    const std::uint16_t mask = binding.*kControls[index].field;
    const bool is_l = kControls[index].field == &GamepadBinding::l, is_r = kControls[index].field == &GamepadBinding::r;
    if (!is_l && !is_r) return wide(button_names(mask));
    const std::wstring trigger = is_l ? L"LT" : L"RT";
    return mask == 0u ? trigger : wide(button_names(mask)) + L" or " + trigger;
}

void paint_controller(HDC dc, const RECT& area, const ControllerView& view, bool dark, float min_label) {
    if (area.right <= area.left || area.bottom <= area.top) return;
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    const Geometry m = geometry_of(area, min_label);
    g.TranslateTransform(static_cast<REAL>(area.left) + m.offset_x, static_cast<REAL>(area.top));
    g.ScaleTransform(m.scale, m.scale);
    Painter painter{g, view, m};
    if (dark) {
        painter.ink = kDarkInk;
        painter.line = kDarkLine;
    }
    painter.draw_pictures();
    painter.draw_labels();
}

int control_at(const RECT& area, POINT point, float min_label) {
    if (area.right <= area.left || area.bottom <= area.top) return -1;
    const Geometry m = geometry_of(area, min_label);
    const PointF p((static_cast<REAL>(point.x - area.left) - m.offset_x) / m.scale, static_cast<REAL>(point.y - area.top) / m.scale);
    if (const int c = hit(kTopHotspots, top_rect(m), p); c >= 0) return c;
    return hit(kTiltedHotspots, tilted_rect(m), p);
}

}  // namespace padconfig
