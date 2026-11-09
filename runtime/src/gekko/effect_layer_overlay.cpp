#include "src/gekko/effect_layer_overlay.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>

#include "assets/texture_import.h"
#include "gekko/effect_layer_keys.h"
#include "render/effect_layers.h"
#include "render/renderer.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma comment(lib, "gdi32.lib")
#endif

namespace gekko {

namespace render = shadow::render;

namespace {

// The atlas: the printable ASCII from ' ' in 16 x 6 cells, rasterised once from the system's Consolas by GDI.
constexpr int kCell = 16, kRow = 24, kColumns = 16, kRowsOfCells = 6;
constexpr int kAtlasWidth = kCell * kColumns, kAtlasHeight = kRow * kRowsOfCells;
// The overlay's canvas is 720 units high; a glyph is drawn at 3/4 of its atlas cell.
constexpr float kCanvasHeight = 720.0f, kScale = 0.75f;
float g_advance = 11.0f;  // the font's advance in atlas pixels (measured)

constexpr std::uint32_t rgba(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}
constexpr std::uint32_t kWhite = rgba(255, 255, 255, 255);
constexpr std::uint32_t kGrey = rgba(130, 130, 130, 255);
constexpr std::uint32_t kYellow = rgba(255, 220, 60, 255);
constexpr std::uint32_t kCyan = rgba(120, 220, 255, 255);

void quad(std::vector<render::ImmediateVertex>& out, float x0, float y0, float x1, float y1, std::uint32_t colour,
          float u0, float v0, float u1, float v1) {
    const render::ImmediateVertex a{{x0, y0, 0}, colour, {u0, v0}}, b{{x1, y0, 0}, colour, {u1, v0}},
        c{{x1, y1, 0}, colour, {u1, v1}}, d{{x0, y1, 0}, colour, {u0, v1}};
    out.insert(out.end(), {a, b, c, a, c, d});
}

}  // namespace

bool EffectLayerOverlay::font(render::Renderer& renderer) {
    if (font_tried_)
        return static_cast<bool>(font_);
    font_tried_ = true;
#if defined(_WIN32)
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = kAtlasWidth;
    info.bmiHeader.biHeight = -kAtlasHeight;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = dc != nullptr ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    HFONT face = CreateFontW(-20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    bool ok = false;
    shadow::assets::TextureImage image;
    if (bitmap != nullptr && face != nullptr) {
        HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
        HGDIOBJ old_font = SelectObject(dc, face);
        std::memset(bits, 0, static_cast<std::size_t>(kAtlasWidth) * kAtlasHeight * 4u);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        SIZE extent{};
        if (GetTextExtentPoint32W(dc, L"M", 1, &extent) && extent.cx > 0)
            g_advance = static_cast<float>(extent.cx);
        for (int ch = 32; ch < 128; ++ch) {
            const wchar_t w = static_cast<wchar_t>(ch);
            TextOutW(dc, ((ch - 32) % kColumns) * kCell, ((ch - 32) / kColumns) * kRow, &w, 1);
        }
        GdiFlush();
        image.width = kAtlasWidth;
        image.height = kAtlasHeight;
        image.mip_levels = 1;
        image.texel_alpha = true;
        image.level_offsets = {0};
        image.rgba.resize(static_cast<std::size_t>(kAtlasWidth) * kAtlasHeight * 4u);
        const auto* bgra = static_cast<const std::uint8_t*>(bits);
        for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
            const std::uint8_t coverage = std::max({bgra[i], bgra[i + 1], bgra[i + 2]});
            image.rgba[i] = image.rgba[i + 1] = image.rgba[i + 2] = 255;
            image.rgba[i + 3] = coverage;
        }
        SelectObject(dc, old_font);
        SelectObject(dc, old_bitmap);
        ok = true;
    }
    if (face != nullptr) DeleteObject(face);
    if (bitmap != nullptr) DeleteObject(bitmap);
    if (dc != nullptr) DeleteDC(dc);
    if (ok) {
        std::string error;
        font_ = renderer.create_texture(render::Scope::Session, image, &error);
        if (!font_)
            std::cerr << "[effect-layers] the overlay font: " << error << '\n';
    }
#else
    (void)renderer;
#endif
    return static_cast<bool>(font_);
}

void EffectLayerOverlay::count_frame() {
    const auto now = std::chrono::steady_clock::now();
    if (fps_frames_ == 0 && fps_window_start_ == std::chrono::steady_clock::time_point{}) {
        fps_window_start_ = fps_last_ = now;
        return;
    }
    const double frame_ms = std::chrono::duration<double, std::milli>(now - fps_last_).count();
    fps_last_ = now;
    ++fps_frames_;
    fps_worst_ms_ = std::max(fps_worst_ms_, frame_ms);
    const double window_ms = std::chrono::duration<double, std::milli>(now - fps_window_start_).count();
    if (window_ms < 500.0)
        return;
    char line[96];
    std::snprintf(line, sizeof(line), "%.1f FPS  %.2f ms  (worst %.2f ms)", 1000.0 * fps_frames_ / window_ms,
                  window_ms / fps_frames_, fps_worst_ms_);
    fps_text_ = line;
    fps_window_start_ = now;
    fps_frames_ = 0;
    fps_worst_ms_ = 0.0;
}

void EffectLayerOverlay::keys(unsigned keys) {
    render::EffectLayerSwitches& switches = render::effect_layer_switches();
    if ((keys & kEffectKeyFps) != 0u)
        set_fps_counter(!fps_);
    if ((keys & kEffectKeyMenu) != 0u)
        visible_ = !visible_;
    if (!visible_)
        return;
    const std::uint32_t last = render::kEffectLayerCount - 1u;
    if ((keys & kEffectKeyUp) != 0u)
        cursor_ = cursor_ > 1u ? cursor_ - 1u : last;
    if ((keys & kEffectKeyDown) != 0u)
        cursor_ = cursor_ < last ? cursor_ + 1u : 1u;
    const auto layer = static_cast<render::EffectLayer>(cursor_);
    if ((keys & kEffectKeyToggle) != 0u)
        switches.toggle(layer);
    if ((keys & kEffectKeySolo) != 0u)
        switches.solo(layer);
    if ((keys & kEffectKeyIsolate) != 0u)
        switches.toggle_isolate();
    if ((keys & kEffectKeyAllOn) != 0u)
        switches.all_on();
    if ((keys & ~kEffectKeyMenu) != 0u)
        std::cout << "[effect-layers] " << switches.describe() << '\n';
}

void EffectLayerOverlay::text(float x, float y, const std::string& line, std::uint32_t colour) {
    const float w = kCell * kScale, h = kRow * kScale;
    for (const char ch : line) {
        const int code = static_cast<unsigned char>(ch);
        if (code > 32 && code < 128) {
            const float u0 = static_cast<float>(((code - 32) % kColumns) * kCell) / kAtlasWidth;
            const float v0 = static_cast<float>(((code - 32) / kColumns) * kRow) / kAtlasHeight;
            quad(glyphs_, x, y, x + w, y + h, colour, u0, v0, u0 + static_cast<float>(kCell) / kAtlasWidth,
                 v0 + static_cast<float>(kRow) / kAtlasHeight);
        }
        x += g_advance * kScale;
    }
}

void EffectLayerOverlay::box(float x0, float y0, float x1, float y1, std::uint32_t colour) {
    quad(panels_, x0, y0, x1, y1, colour, 0, 0, 0, 0);
}

void EffectLayerOverlay::append(render::FramePacket& packet, render::Renderer& renderer, std::uint32_t output_width,
                                std::uint32_t output_height) {
    const render::EffectLayerState state = render::effect_layer_switches().state();
    if (fps_)
        count_frame();
    if (!visible_ && state.everything() && !fps_)
        return;
    if (!font(renderer))
        return;
    glyphs_.clear();
    panels_.clear();
    const float line = kRow * kScale;
    const float x = 24.0f;
    float y = 24.0f;
    if (!visible_ && !state.everything()) {
        const std::string status = "Effects: " + render::effect_layer_switches().describe() + "   (Num / or F6: layer list)";
        box(x - 8.0f, y - 4.0f, x + 8.0f + g_advance * kScale * static_cast<float>(status.size()), y + line + 2.0f,
            rgba(0, 0, 0, 160));
        text(x, y, status, kYellow);
    } else if (visible_) {
        const std::string help = "EFFECT LAYERS (Num Lock on)  Num / hide | Num 8 / Num 2 move | Num 5 on/off | Num 0 solo"
                                 " | Num . world in solo | Num - all on";
        const std::uint32_t rows = render::kEffectLayerCount + 2u;
        box(x - 10.0f, y - 6.0f, x + 10.0f + g_advance * kScale * static_cast<float>(help.size()),
            y + line * static_cast<float>(rows) + 8.0f, rgba(0, 0, 0, 170));
        text(x, y, help, kCyan);
        y += line;
        for (std::uint32_t i = 1; i < render::kEffectLayerCount; ++i, y += line) {
            const auto layer = static_cast<render::EffectLayer>(i);
            const bool drawn = state.drawn(layer);
            const bool solo = state.solo == layer;
            if (i == cursor_)
                box(x - 4.0f, y, x + g_advance * kScale * 40.0f, y + line, rgba(60, 90, 160, 200));
            const std::string mark = solo ? "[SOLO]" : drawn ? "[ on ]" : "[ off]";
            text(x, y, std::string(i == cursor_ ? "> " : "  ") + mark + " " + render::effect_layer_title(layer) +
                           "  (" + render::effect_layer_name(layer) + ")",
                 solo ? kYellow : drawn ? kWhite : kGrey);
        }
        std::string status = render::effect_layer_switches().describe();
        if (state.solo != render::EffectLayer::None)
            status += state.isolate ? "  - Num .: show the world" : "  - Num .: hide the world";
        text(x, y, status, kYellow);
    }
    // One view over everything the game drew, its canvas 720 units high in the output's shape.
    const float aspect = output_width != 0u && output_height != 0u
                             ? static_cast<float>(output_width) / static_cast<float>(output_height)
                             : 16.0f / 9.0f;
    if (fps_ && !fps_text_.empty()) {
        // Top right: the presented frames per second and frame times of the last half second.
        const float w = g_advance * kScale * static_cast<float>(fps_text_.size());
        const float fx = kCanvasHeight * aspect - 24.0f - w;
        box(fx - 8.0f, 20.0f, fx + w + 8.0f, 24.0f + line + 2.0f, rgba(0, 0, 0, 160));
        text(fx, 24.0f, fps_text_, kWhite);
    }
    render::ViewRecord view;
    view.canvas = render::Canvas{kCanvasHeight * aspect, kCanvasHeight, 0.0f};
    view.over_views = true;
    const auto view_id = static_cast<render::ViewId>(packet.views.size());
    packet.views.push_back(view);
    render::PassRecord pass;
    pass.view = view_id;
    pass.kind = render::PassKind::Scene;
    pass.first_draw = static_cast<std::uint32_t>(packet.draws.size());
    render::Recipe recipe;
    recipe.src = render::Blend::SrcAlpha;
    recipe.dst = render::Blend::InvSrcAlpha;
    recipe.depth_test = false;
    recipe.depth_write = false;
    recipe.cull = render::Cull::None;
    const auto add = [&](const std::vector<render::ImmediateVertex>& vertices, render::TextureHandle texture) {
        if (vertices.empty())
            return;
        render::ImmediateDraw draw;
        draw.topology = render::Topology::Triangles;
        draw.space = render::Space::Canvas;
        draw.anchor = render::Anchor::Left;
        draw.first_vertex = static_cast<std::uint32_t>(packet.immediate_vertices.size());
        draw.vertex_count = static_cast<std::uint32_t>(vertices.size());
        draw.first_index = static_cast<std::uint32_t>(packet.immediate_indices.size());
        draw.texture = texture;
        draw.sampler = render::Sampler{render::Filter::Linear, render::Address::Clamp, render::Address::Clamp};
        packet.immediate_vertices.insert(packet.immediate_vertices.end(), vertices.begin(), vertices.end());
        render::DrawRecord record;
        record.recipe = recipe;
        record.immediate = static_cast<std::uint32_t>(packet.immediates.size());
        packet.immediates.push_back(draw);
        packet.draws.push_back(record);
        ++pass.draw_count;
    };
    add(panels_, render::TextureHandle{});
    add(glyphs_, font_);
    packet.passes.push_back(pass);
}

}  // namespace gekko
