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
    static std::map<int, Bitmap*> cache;
    auto it = cache.find(id);
    if (it == cache.end()) it = cache.emplace(id, load_art(id)).first;
    return it->second;
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

// ---- the sticks: the picture from above in layers (the body, and each cap turned on a bone, in frames), drawn the way
// the player tilts the stick, in a GameCube stick's octagonal gate ----------------------------------------------------

constexpr REAL kPi = 3.14159265f;
constexpr REAL kSector = kPi / 8.0f;  // half the angle between two notches (22.5 degrees)

// How far the octagonal gate lets the stick go towards `angle` (radians from the right, anticlockwise): 1 at a notch
// (its corners, every 45 degrees), cos(22.5) halfway between two.
REAL gate_at(REAL angle) {
    REAL off = std::fmod(angle, kPi / 4.0f);
    if (off < 0) off += kPi / 4.0f;
    return std::cos(kSector) / std::cos(off - kSector);
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

    // A stick cap as the stick is held (x, y: in its gate, stick_in_gate()). The sheets have frames at 4 tilts in 16
    // directions; the stick is mostly between them, so the (up to) four frames round it are blended by how near each is:
    // first the dome below the cap, where it is (it turns in place), then the cap, every frame moved so that its top
    // (kStickTops, measured in the render) lands on the same point. The top glides with the stick and the shading turns
    // smoothly, with no step from one frame to the next and nothing doubled. A red ring
    // like a pressed button's rounds the top, stronger the further the stick is tilted.
    void draw_cap(const CapArt& cap, int index, float x, float y) {
        Bitmap* dome_sheet = art(cap.id);
        Bitmap* cap_sheet = art(cap.cap_id);
        Bitmap* picture = art(101);
        if (dome_sheet == nullptr || cap_sheet == nullptr || picture == nullptr) return;
        const REAL length = std::sqrt(x * x + y * y);
        // Where the stick is between the frames: tilt in rings (0 to kStickRings), direction in frame steps.
        REAL ring_at = 0.0f, direction_at = 0.0f;
        if (length > 0.0f) {
            REAL angle = std::atan2(y, x);
            if (angle < 0.0f) angle += 2.0f * kPi;
            ring_at = std::clamp(length / gate_at(angle) * kStickRings, 0.0f, static_cast<REAL>(kStickRings));
            direction_at = angle / (2.0f * kPi) * kStickDirections;
        }
        const int r0 = std::min(static_cast<int>(ring_at), kStickRings - 1), d0 = static_cast<int>(direction_at);
        const REAL tr = ring_at - r0, td = direction_at - d0;
        struct Frame {
            int ring, direction;
            REAL weight;
        };
        Frame frames[4];
        int count = 0;
        const auto add = [&](int ring, int direction, REAL weight) {
            if (weight <= 1.0f / 256.0f) return;
            direction = ring == 0 ? 0 : direction % kStickDirections;
            for (int i = 0; i < count; ++i)
                if (frames[i].ring == ring && frames[i].direction == direction) {
                    frames[i].weight += weight;
                    return;
                }
            frames[count++] = {ring, direction, weight};
        };
        add(r0, d0, (1.0f - tr) * (1.0f - td));
        add(r0, d0 + 1, (1.0f - tr) * td);
        add(r0 + 1, d0, tr * (1.0f - td));
        add(r0 + 1, d0 + 1, tr * td);
        if (count == 0) add(0, 0, 1.0f);
        std::sort(frames, frames + count, [](const Frame& a, const Frame& b) { return a.weight > b.weight; });
        // The top now: the frames' own tops, blended the same way.
        const auto top_of = [&](const Frame& f) {
            const float* p = kStickTops[index][f.ring][f.direction];
            return PointF(p[0], p[1]);
        };
        REAL total = 0.0f;
        PointF now(0.0f, 0.0f);
        for (int i = 0; i < count; ++i) {
            const PointF p = top_of(frames[i]);
            now.X += p.X * frames[i].weight;
            now.Y += p.Y * frames[i].weight;
            total += frames[i].weight;
        }
        now.X /= total;
        now.Y /= total;
        const REAL art_w = static_cast<REAL>(picture->GetWidth()), art_h = static_cast<REAL>(picture->GetHeight());
        const REAL cell_w = static_cast<REAL>(cap_sheet->GetWidth()) / kStickDirections;
        const REAL cell_h = static_cast<REAL>(cap_sheet->GetHeight()) / (kStickRings + 1);
        const RectF r = top_rect(m);
        const REAL sx = r.Width / art_w, sy = r.Height / art_h;  // the picture's pixels to view units
        // The heaviest frame opaque, each next one over it by its share of the weight drawn so far: together, each frame
        // by its weight. A frame's size comes from the sheet itself, at the picture's scale.
        const auto blend = [&](Bitmap* sheet, bool on_top) {
            REAL drawn = 0.0f;
            for (int i = 0; i < count; ++i) {
                const Frame& f = frames[i];
                drawn += f.weight;
                const PointF own = top_of(f);
                const REAL dx = on_top ? now.X - own.X : 0.0f, dy = on_top ? now.Y - own.Y : 0.0f;
                const RectF box(r.X + (cap.x * art_w + dx) * sx, r.Y + (cap.y * art_h + dy) * sy, cell_w * sx, cell_h * sy);
                const REAL source_x = f.direction * cell_w, source_y = f.ring * cell_h;
                if (i == 0) {
                    g.DrawImage(sheet, box, source_x, source_y, cell_w, cell_h, UnitPixel);
                    continue;
                }
                ColorMatrix fade = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, f.weight / drawn, 0}, {0, 0, 0, 0, 1}}};
                ImageAttributes attributes;
                attributes.SetColorMatrix(&fade);
                g.DrawImage(sheet, box, source_x, source_y, cell_w, cell_h, UnitPixel, &attributes);
            }
        };
        blend(dome_sheet, false);
        blend(cap_sheet, true);
        if (length == 0.0f) return;
        const REAL amount = std::min(1.0f, length);
        const REAL radius = r.Width * cap.top;
        const PointF top(r.X + now.X * sx, r.Y + now.Y * sy);
        Pen ring_pen(Color(static_cast<BYTE>(90 + 120 * amount), 235, 45, 35), 3.0f);
        g.DrawEllipse(&ring_pen, top.X - radius, top.Y - radius, radius * 2.0f, radius * 2.0f);
    }    void draw_pictures() {
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        // Still sticks: the whole picture. A tilted one: the body without its caps, and the caps where the sticks are.
        const bool tilted = v.main_x != 0.0f || v.main_y != 0.0f || v.c_x != 0.0f || v.c_y != 0.0f;
        Bitmap* body = tilted ? art(kTopBodyId) : nullptr;
        if (body != nullptr) {
            g.DrawImage(body, top_rect(m));
            draw_cap(kStickCaps[0], 0, v.main_x, v.main_y);
            draw_cap(kStickCaps[1], 1, v.c_x, v.c_y);
        } else if (Bitmap* b = art(101)) {
            g.DrawImage(b, top_rect(m));
        }
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

void stick_in_gate(float x, float y, float& gate_x, float& gate_y) {
    const REAL length = std::min(1.0f, std::sqrt(x * x + y * y));
    if (length <= 0.0f) {
        gate_x = gate_y = 0.0f;
        return;
    }
    REAL angle = std::atan2(y, x);
    // Pushed (nearly) all the way, the stick slides along the gate into a notch it is within 8 degrees of.
    const REAL notch = std::round(angle / (kPi / 4.0f)) * (kPi / 4.0f);
    if (length > 0.85f && std::fabs(angle - notch) < kPi / 22.5f) angle = notch;
    const REAL held = std::min(length, gate_at(angle));
    gate_x = held * std::cos(angle);
    gate_y = held * std::sin(angle);
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
