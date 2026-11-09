#pragma once

// The one display rule (docs/DISPLAY_SETTINGS_AUDIT.md section 6). The player's settings, each with one job:
//
//   display mode       windowed (a normal window, any size the player drags it to; the size is remembered) or
//                      fullscreen (the desktop's size)
//   screen ratio       the game picture's shape: Auto (the window's / screen's own) or a fixed ratio
//   resolution scale   the 3D scene's pixels per side of the picture, 25..200 %
//
// and from the output the window owner actually reached (the client area or the monitor), in this order:
//
//   picture = the largest rectangle of the ratio inside the output, centred, black bars outside (Auto: the whole
//             output). A ratio within 3 % of the output's own shape is the output's shape: no hairline bars.
//   render  = picture x the scale - lowered by one factor on both sides only where a side would pass the D3D12
//             texture limit (reported: scale_capped)
//   the present pass maps the render onto the picture: the only resampling (1:1, a 2 x 2 box at exactly 2:1, a
//   bicubic otherwise)
//
// The picture has this one shape on every frame - menus, movies, cutscenes and gameplay alike (4:3 content keeps
// its own box inside it, as the game composes it). The camera, culling and HUD take the picture's exact aspect. No
// setting changes another's value: the renderer's size depends on all three, the settings do not depend on each
// other.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace gekko {

struct PixelSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

inline constexpr std::uint32_t kMaxRenderSide = 16384u;  // D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION
inline constexpr std::uint32_t kMinPictureWidth = 320u;  // the smallest window client (the picture never collapses)
inline constexpr std::uint32_t kMinPictureHeight = 240u;
inline constexpr double kMinRenderScale = 25.0;          // percent
inline constexpr double kMaxRenderScale = 200.0;
inline constexpr double kDefaultRenderScale = 100.0;

// The ratios offered by name. Each is its exact ratio: "21:9" is 7:3 = 2.333:1 - the ultrawide panels sold as 21:9 are
// 2.370 (2560 x 1080) and 2.389 (3440 x 1440); Auto (the screen's own shape) fits them exactly.
struct NamedAspect {
    const char* name;
    std::uint32_t w, h;
};
inline constexpr NamedAspect kNamedAspects[] = {{"4:3", 4, 3},    {"5:4", 5, 4},     {"16:10", 16, 10}, {"16:9", 16, 9},
                                                {"21:9", 21, 9}, {"32:10", 32, 10}, {"32:9", 32, 9}};

inline double named_aspect(const char* name) noexcept {
    if (name == nullptr) return 0.0;
    for (const NamedAspect& a : kNamedAspects)
        if (std::strcmp(a.name, name) == 0) return static_cast<double>(a.w) / a.h;
    return 0.0;
}

// The name of a ratio given as a number (nullptr: not one of the named ones).
inline const char* aspect_name(double aspect) noexcept {
    for (const NamedAspect& a : kNamedAspects)
        if (std::fabs(aspect - static_cast<double>(a.w) / a.h) < 1e-6) return a.name;
    return nullptr;
}

// A ratio as people read it: "16:9" when it is a named one exactly, else "2.39:1".
inline std::string aspect_label(double aspect) {
    if (const char* name = aspect_name(aspect)) return name;
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f:1", aspect);
    return text;
}

// One parser for an aspect value, shared by display.cfg and the command line: "auto" / "0" (0 = Auto), a named or any
// W:H ratio ("32:10", "64:27"), "WxH", or a number ("2.37"); 0.25..8. A value within float precision of a named ratio
// is that ratio exactly. False: not an aspect.
inline bool parse_aspect_value(std::string text, double* out) noexcept {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto first = text.find_first_not_of(" \t");
    const auto last = text.find_last_not_of(" \t");
    text = first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
    if (text == "auto" || text == "0") {
        *out = 0.0;
        return true;
    }
    if (text == "native") text = "4:3";  // the legacy spelling of the console's shape
    double ratio = 0.0;
    const std::size_t split = text.find_first_of(":x");
    char* end = nullptr;
    if (split != std::string::npos) {
        const double w = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + split) return false;
        const char* h_text = text.c_str() + split + 1;
        const double h = std::strtod(h_text, &end);
        if (end == h_text || *end != '\0' || !(h > 0.0)) return false;
        ratio = w / h;
    } else {
        ratio = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || *end != '\0') return false;
    }
    if (!(ratio >= 0.25 && ratio <= 8.0)) return false;
    for (const NamedAspect& a : kNamedAspects) {
        const double exact = static_cast<double>(a.w) / a.h;
        if (std::fabs(ratio - exact) < 1e-5) ratio = exact;
    }
    *out = ratio;
    return true;
}

// The largest rectangle of `aspect` inside `outer` (the whole of it when aspect <= 0).
inline PixelSize fit_aspect(PixelSize outer, double aspect) noexcept {
    if (outer.width == 0u || outer.height == 0u || !(aspect > 0.0)) return outer;
    const double outer_aspect = static_cast<double>(outer.width) / outer.height;
    PixelSize fit = outer;
    if (outer_aspect > aspect)  // wider than the picture: the height limits, bars at the sides
        fit.width = static_cast<std::uint32_t>(std::lround(outer.height * aspect));
    else                        // taller: the width limits, bars above and below
        fit.height = static_cast<std::uint32_t>(std::lround(outer.width / aspect));
    fit.width = std::clamp<std::uint32_t>(fit.width, 1u, outer.width);
    fit.height = std::clamp<std::uint32_t>(fit.height, 1u, outer.height);
    return fit;
}

// The largest scale (percent, <= `percent`) at which both sides of `size` stay within kMaxRenderSide: one factor for
// both sides, so the shape is kept.
inline double capped_scale(PixelSize size, double percent) noexcept {
    double s = percent;
    const std::uint32_t side = std::max(size.width, size.height);
    if (side != 0u) s = std::min(s, 100.0 * static_cast<double>(kMaxRenderSide) / static_cast<double>(side));
    return s;
}

// `size` scaled by `percent` per side, each side at least 1. The caller caps the percent first (capped_scale); a side
// that would still exceed kMaxRenderSide through rounding is clamped (at most one pixel).
inline PixelSize scale_size(PixelSize size, double percent) noexcept {
    const double s = percent / 100.0;
    const auto side = [&](std::uint32_t v) {
        const long scaled = std::lround(static_cast<double>(v) * s);
        return static_cast<std::uint32_t>(std::clamp<long>(scaled, 1L, static_cast<long>(kMaxRenderSide)));
    };
    return {side(size.width), side(size.height)};
}

// Within this much of the output's own shape a fixed ratio is the output's shape (a window dragged to almost the
// ratio, a 3440 x 1440 panel with 21:9 chosen): the picture fills it, no bars of a few pixels.
inline constexpr double kRatioSnap = 0.03;  // 3440x1440 is 2.4 % from 21:9; named ratios are >= 6.7 % apart

struct DisplayGeometry {
    PixelSize output;              // what the picture is shown in: the client area or the monitor
    PixelSize picture;             // the game picture inside it
    std::uint32_t picture_x = 0;   // its offset in the output (centred)
    std::uint32_t picture_y = 0;
    double aspect = 0.0;           // the picture's exact shape: the chosen ratio, else the output's
    PixelSize render;              // the 3D render size
    double asked_scale = 100.0;    // the resolution scale setting
    double effective_scale = 100.0;  // the scale in effect (asked_scale unless capped)
    bool scale_capped = false;     // the asked scale would pass the texture limit: lowered uniformly
};

// `ratio`: the screen ratio (0 = Auto: the output's own). `scale_percent` <= 0: 100 %.
inline DisplayGeometry display_geometry(PixelSize output, double ratio, double scale_percent) noexcept {
    DisplayGeometry g;
    g.output = {std::max<std::uint32_t>(output.width, 1u), std::max<std::uint32_t>(output.height, 1u)};
    const double output_aspect = static_cast<double>(g.output.width) / g.output.height;
    const bool fixed = ratio > 0.0 && std::fabs(ratio / output_aspect - 1.0) > kRatioSnap;
    g.aspect = fixed ? ratio : output_aspect;
    g.picture = fixed ? fit_aspect(g.output, ratio) : g.output;
    g.picture_x = (g.output.width - g.picture.width) / 2u;
    g.picture_y = (g.output.height - g.picture.height) / 2u;
    g.asked_scale = scale_percent > 0.0 ? scale_percent : kDefaultRenderScale;
    g.effective_scale = capped_scale(g.picture, g.asked_scale);
    g.scale_capped = g.effective_scale < g.asked_scale;
    g.render = scale_size(g.picture, g.effective_scale);
    return g;
}

// The window a fixed ratio gives a windowed client of `client` when the player picks it: the same width, the height
// of the ratio - smaller on both sides when that does not fit `work` (the largest client the desktop holds; 0 x 0:
// not known). Auto: the client as it is.
inline PixelSize window_for_ratio(PixelSize client, double ratio, PixelSize work) noexcept {
    if (!(ratio > 0.0) || client.width == 0u) return client;
    PixelSize shaped{client.width, static_cast<std::uint32_t>(std::max(1L, std::lround(client.width / ratio)))};
    if (work.width != 0u && work.height != 0u && (shaped.width > work.width || shaped.height > work.height))
        shaped = fit_aspect(work, ratio);
    return shaped;
}

// The bytes the scene's main targets of a render size take (render/renderer.cpp: multisampled RGBA16 colour + D32
// depth, plus the resolved view, glare sources, snapshot, history and the composed output at one sample) - the
// estimate the render size is checked against the GPU's memory with. 64-bit: no overflow at any accepted size.
inline std::uint64_t render_target_bytes(PixelSize render, std::uint32_t msaa_samples) noexcept {
    const std::uint64_t pixels = static_cast<std::uint64_t>(render.width) * render.height;
    const std::uint64_t samples = std::max<std::uint32_t>(msaa_samples, 1u);
    return pixels * (samples * (8u + 4u) + 8u + 8u + 8u + 8u + 4u);
}

}  // namespace gekko
