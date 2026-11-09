#include "src/gekko/options_display_overlay.h"

#include <algorithm>
#include <iostream>
#include <mutex>

#include "assets/texture_import.h"
#include "gekko/native_options_display.h"
#include "render/renderer.h"

namespace gekko {

namespace render = shadow::render;
namespace od = options_display;

namespace {

constexpr float kCanvasWidth = 640.0f, kCanvasHeight = 480.0f;
// fontnull text1..text7 (Options.gncp, the page open): x 399, y 140 + 40 n. The text's origin sits 3 px right of and
// 5 px above the slot (measured on the Dolphin reference, tools/options_menu_sim).
constexpr float kSlotX = 399.0f, kSlotY = 140.0f, kSlotStep = 40.0f;
constexpr float kTextX = kSlotX + 3.0f, kTextLift = 5.0f;
constexpr float kValueRight = 622.0f;  // the panel's right edge for right-aligned values
constexpr float kLabelScale = 0.85f, kGap = 12.0f, kSpace = 6.0f;
// The game's font draw (0x8004A09C) draws every string twice, both with the glyph's texel times its colour:
//   * the shadow first, at (+0.005, -0.005) of its y-up 2D space (the canvas's +3.2, +2.4 px), blended ZERO /
//     INVSRCCOLOR: the picture times (1 - colour), so a yellow glyph leaves a blue shadow and a white one a black one;
//   * then the glyph at its place, blended ONE / ONE: added onto the picture.
// The sheets' black adds nothing and darkens nothing, so the glyph cells need no alpha.
constexpr float kShadowX = 0.005f * kCanvasWidth, kShadowY = 0.005f * kCanvasHeight;

constexpr std::uint32_t rgba(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}
// The page's text colours as the game sets them (0x802BF108(node, colour, brightness)): its table 0x80558AE8 gives
// colour 0 white for a row and colour 2 yellow (FFFF00) for the selected one, each channel times the brightness and
// truncated, alpha 255. The rows draw at brightness 1.0 while the page has the cursor and 0.5 otherwise
// (0x802B8BC4's flag: K(-0xCBC) / K(-0xCB8)).
constexpr std::uint32_t kRowColour = 0xFFFFFFu, kSelectedColour = 0xFFFF00u;  // RGB
constexpr std::uint32_t text_colour(std::uint32_t rgb, bool page_has_cursor) {
    const std::uint32_t r = rgb >> 16 & 0xFFu, g = rgb >> 8 & 0xFFu, b = rgb & 0xFFu;
    return page_has_cursor ? rgba(r, g, b, 255) : rgba(r / 2u, g / 2u, b / 2u, 255);
}

void quad(std::vector<render::ImmediateVertex>& out, float x0, float y0, float x1, float y1, std::uint32_t colour,
          float u0, float v0, float u1, float v1) {
    const render::ImmediateVertex a{{x0, y0, 0}, colour, {u0, v0}}, b{{x1, y0, 0}, colour, {u1, v0}},
        c{{x1, y1, 0}, colour, {u1, v1}}, d{{x0, y1, 0}, colour, {u0, v1}};
    out.insert(out.end(), {a, b, c, a, c, d});
}

const od::Font& font_of(const void* p) { return *static_cast<const od::Font*>(p); }

}  // namespace

// Latin sheets have fixed 12 px cells and are spaced by their ink plus one pixel; wider cells (the JP sheets) advance
// by their cell. The sheets have no '%' (built from '/' and two half-size 'o's) and EN has no ink for ':' (two '.').
float OptionsDisplayOverlay::width(const std::u16string& s, float scale) const {
    const od::Font& f = font_of(font_);
    float w = 0.0f;
    for (const char16_t c : s) {
        const od::Glyph* g = f.find(c);
        if (c == u' ') {
            w += kSpace;
        } else if (g != nullptr) {
            w += g->width >= 24u ? g->width : static_cast<float>(g->ink_right - g->ink_left) + 1.0f;
        } else if (c == u'%' && f.find(u'/') && f.find(u'o')) {
            w += width(u"/", 1.0f) + width(u"o", 1.0f) * 0.4f + 1.0f;
        } else if (c == u':' && f.find(u'.')) {
            w += width(u".", 1.0f);
        } else {
            w += kSpace;
        }
    }
    return w * scale;
}

void OptionsDisplayOverlay::glyph(char16_t c, float& x, float y, float scale, std::uint32_t colour) {
    const od::Font& f = font_of(font_);
    if (c == u' ') {
        x += kSpace * scale;
        return;
    }
    const od::Glyph* g = f.find(c);
    if (g == nullptr) {
        if (c == u'%' && f.find(u'/') && f.find(u'o')) {
            const float slash = width(u"/", scale), o = width(u"o", scale * 0.5f);
            float ox = x;
            glyph(u'o', ox, y - 5.0f * scale, scale * 0.5f, colour);
            ox = x + o * 0.4f;
            glyph(u'/', ox, y, scale, colour);
            ox = x + o * 0.4f + slash - o * 0.6f;
            glyph(u'o', ox, y + 5.0f * scale, scale * 0.5f, colour);
            x += o * 0.4f + slash + o * 0.4f + scale;
        } else if (c == u':' && f.find(u'.')) {
            float ox = x;
            glyph(u'.', ox, y - 8.0f * scale, scale, colour);
            glyph(u'.', x, y, scale, colour);
        } else {
            x += kSpace * scale;
        }
        return;
    }
    const bool wide = g->width >= 24u;  // the JP sheets' wide cells; Hungarian letter boxes are 4..14 px (ink-drawn)
    const float sx = static_cast<float>(wide ? g->x : g->x + g->ink_left);
    const float sw = static_cast<float>(wide ? g->width : std::max<int>(1, g->ink_right - g->ink_left));
    const float sh = g->height;
    const float tw = static_cast<float>(sheet_sizes_[g->sheet * 2u]), th = static_cast<float>(sheet_sizes_[g->sheet * 2u + 1u]);
    auto run = std::find_if(runs_.begin(), runs_.end(), [&](const Run& r) { return r.sheet == g->sheet; });
    if (run == runs_.end()) {
        runs_.push_back(Run{g->sheet, {}, {}});
        run = runs_.end() - 1;
    }
    const float y0 = y - sh * scale * 0.5f;
    quad(run->shadow, x + kShadowX, y0 + kShadowY, x + sw * scale + kShadowX, y0 + sh * scale + kShadowY, colour,
         sx / tw, g->y / th, (sx + sw) / tw, (g->y + sh) / th);
    quad(run->text, x, y0, x + sw * scale, y0 + sh * scale, colour, sx / tw, g->y / th, (sx + sw) / tw,
         (g->y + sh) / th);
    x += (wide ? sw : sw + 1.0f) * scale;
}

void OptionsDisplayOverlay::text(const std::u16string& s, float x, float y, float scale, std::uint32_t colour) {
    for (const char16_t c : s) glyph(c, x, y, scale, colour);
}

void OptionsDisplayOverlay::append(render::FramePacket& packet, render::Renderer& renderer) {
    od::Shared& shared = od::shared();
    std::lock_guard<std::mutex> lock(shared.mutex);
    // The page of the game frame this packet is (not the newest one: the game runs ahead of the present).
    const od::Page* drawn = shared.at(packet.game_ticks);
    if (drawn == nullptr || shared.font.glyphs.empty()) return;
    const od::Page& page = *drawn;
    if (shared.font.serial != font_serial_) {
        sheets_.clear();
        sheet_sizes_.clear();
        for (const od::Font::Sheet& sheet : shared.font.sheets) {
            shadow::assets::TextureImage image;
            image.width = sheet.width;
            image.height = sheet.height;
            image.mip_levels = 1;
            image.texel_alpha = false;  // opaque: blended by colour
            image.rgba = sheet.rgba;
            image.level_offsets = {0};
            std::string error;
            const render::TextureHandle handle = renderer.create_texture(render::Scope::Session, image, &error);
            if (!handle) std::cerr << "[options-display] a glyph sheet: " << error << '\n';
            sheets_.push_back(handle);
            sheet_sizes_.push_back(sheet.width);
            sheet_sizes_.push_back(sheet.height);
        }
        font_serial_ = shared.font.serial;
    }
    font_ = &shared.font;
    runs_.clear();
    if (!page.dialog.empty()) {
        // The display confirmation: its lines in the page's slots, the question in the selected colour.
        for (std::uint32_t i = 0; i < page.dialog.size() && i < od::kVisibleRows; ++i) {
            const float y = kSlotY + kSlotStep * static_cast<float>(i) - kTextLift;
            const std::uint32_t colour = text_colour(i == 0u ? kSelectedColour : kRowColour, true);
            float scale = kLabelScale;
            const float need = width(page.dialog[i], scale);
            if (need > kValueRight - kTextX) scale *= (kValueRight - kTextX) / need;
            text(page.dialog[i], kTextX, y, scale, colour);
        }
    } else {
        const std::uint32_t first = page.scroll;
        for (std::uint32_t i = 0; i < od::kVisibleRows && first + i < page.lines.size(); ++i) {
            const od::Page::Line& line = page.lines[first + i];
            const float y = kSlotY + kSlotStep * static_cast<float>(i) - kTextLift;
            const bool cursor = page.focused && first + i == page.cursor;
            // A choice shows "< value >" under the cursor; an action or a list entry is its label alone; an
            // information row shows its value and never takes the selected colour's arrows.
            const bool choice = !line.value.empty() && !line.info;
            const std::u16string value = cursor && choice ? u"< " + line.value + u" >" : line.value;
            float scale = kLabelScale;
            const float need = width(line.label, scale) + (value.empty() ? 0.0f : width(value, scale) + kGap);
            if (need > kValueRight - kTextX) scale *= (kValueRight - kTextX) / need;
            const std::uint32_t colour = text_colour(cursor ? kSelectedColour : kRowColour, page.focused);
            text(line.label, kTextX, y, scale, colour);
            if (!value.empty()) text(value, kValueRight - width(value, scale), y, scale, colour);
        }
        // The scroll position: "n/m" above the first slot, right-aligned, and a mark where more rows are hidden.
        if (!page.counter.empty() && page.focused) {
            const float scale = 0.6f;
            const float y = kSlotY - kSlotStep * 0.62f;
            text(page.counter, kValueRight - width(page.counter, scale), y, scale, text_colour(kRowColour, true));
            const std::uint32_t mark = text_colour(kSelectedColour, true);
            if (page.more_above) text(u"-", kTextX, y, scale, mark);
            if (page.more_below) {
                const float yb = kSlotY + kSlotStep * (static_cast<float>(od::kVisibleRows) - 0.45f) - kTextLift;
                text(u"...", kTextX, yb, scale, mark);
            }
        }
    }
    font_ = nullptr;

    render::ViewRecord view;
    view.canvas = render::Canvas{kCanvasWidth, kCanvasHeight, kCanvasWidth / kCanvasHeight};
    view.over_views = true;
    const auto view_id = static_cast<render::ViewId>(packet.views.size());
    packet.views.push_back(view);
    render::PassRecord pass;
    pass.view = view_id;
    pass.kind = render::PassKind::Scene;
    pass.first_draw = static_cast<std::uint32_t>(packet.draws.size());
    render::Recipe recipe;
    recipe.depth_test = false;
    recipe.depth_write = false;
    recipe.cull = render::Cull::None;
    const auto add = [&](const std::vector<render::ImmediateVertex>& vertices, render::TextureHandle texture,
                         render::Blend src, render::Blend dst) {
        if (vertices.empty()) return;
        render::ImmediateDraw draw;
        draw.topology = render::Topology::Triangles;
        draw.space = render::Space::Canvas;
        draw.anchor = render::Anchor::Center;
        draw.first_vertex = static_cast<std::uint32_t>(packet.immediate_vertices.size());
        draw.vertex_count = static_cast<std::uint32_t>(vertices.size());
        draw.first_index = static_cast<std::uint32_t>(packet.immediate_indices.size());
        draw.texture = texture;
        draw.sampler = render::Sampler{render::Filter::Linear, render::Address::Clamp, render::Address::Clamp};
        packet.immediate_vertices.insert(packet.immediate_vertices.end(), vertices.begin(), vertices.end());
        render::DrawRecord record;
        record.recipe = recipe;
        record.recipe.src = src;
        record.recipe.dst = dst;
        record.immediate = static_cast<std::uint32_t>(packet.immediates.size());
        packet.immediates.push_back(draw);
        packet.draws.push_back(record);
        ++pass.draw_count;
    };
    for (const Run& run : runs_)
        if (run.sheet < sheets_.size() && sheets_[run.sheet])
            add(run.shadow, sheets_[run.sheet], render::Blend::Zero, render::Blend::InvSrcColor);
    for (const Run& run : runs_)
        if (run.sheet < sheets_.size() && sheets_[run.sheet])
            add(run.text, sheets_[run.sheet], render::Blend::One, render::Blend::One);
    packet.passes.push_back(pass);
}

}  // namespace gekko
