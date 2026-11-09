#include "gekko/camera.h"
#include "gekko/display.h"

#include "gekko/host_jobs.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace gekko {
namespace {

std::string trim_copy(std::string text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string lower_copy(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool parse_bool(const std::string& text, bool* value) {
    const std::string lowered = lower_copy(text);
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
        *value = true;
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        *value = false;
        return true;
    }
    return false;
}

bool parse_int(const std::string& text, int* value) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }
    *value = static_cast<int>(parsed);
    return true;
}

bool parse_float(const std::string& text, float* value) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    *value = parsed;
    return true;
}

}  // namespace

float aspect_ratio_for_mode(AspectMode mode, float custom, int output_width,
                            int output_height) noexcept {
    switch (mode) {
        case AspectMode::FourThree:
            return 4.0f / 3.0f;
        case AspectMode::SixteenNine:
            return 16.0f / 9.0f;
        case AspectMode::SixteenTen:
            return 16.0f / 10.0f;
        case AspectMode::TwentyOneNine:
            return 21.0f / 9.0f;
        case AspectMode::ThirtyTwoNine:
            return 32.0f / 9.0f;
        case AspectMode::Custom:
            return (custom > 0.1f && std::isfinite(custom)) ? custom : kNativeAspect;
        case AspectMode::Auto:
        default:
            if (output_width > 0 && output_height > 0) {
                return static_cast<float>(output_width) / static_cast<float>(output_height);
            }
            return kNativeAspect;
    }
}

float horplus_m00_scale(float target_aspect) noexcept {
    // The ONE Hor+ implementation lives in gekko/camera.h, where it is a change
    // to a physical frustum extent. This is the legacy raster's adapter to it:
    // load_matrices multiplies the projection coefficient m00, which is the
    // reciprocal of the horizontal extent, so the scale it needs is the
    // reciprocal of the extent scale.
    //
    // Exactly 1.0f at and below 4:3 either way, which is what keeps 640x480
    // byte-identical.
    return camera::Coverage::from_aspect(target_aspect).affine_scale();
}

float render_content_aspect(const DisplayLayout& layout) noexcept {
    if (!(layout.aspect > 0.1f) || !std::isfinite(layout.aspect)) {
        return kNativeAspect;
    }
    return std::max(layout.aspect, kNativeAspect);
}

bool layout_is_native(const DisplayLayout& layout) noexcept {
    return std::fabs(layout.aspect - kNativeAspect) < 1.0e-4f &&
           std::fabs(layout.hud_scale - 1.0f) < 1.0e-4f && layout.hud_inset <= 0.0f;
}

DisplayLayout make_display_layout(const DisplayConfig& config, int output_width,
                                  int output_height) noexcept {
    DisplayLayout layout;
    layout.output_width = config.render_width > 0 ? config.render_width : output_width;
    layout.output_height = config.render_height > 0 ? config.render_height : output_height;
    if (layout.output_width <= 0) {
        layout.output_width = 640;
    }
    if (layout.output_height <= 0) {
        layout.output_height = 480;
    }
    layout.aspect = aspect_ratio_for_mode(config.aspect, config.custom_aspect,
                                          layout.output_width, layout.output_height);
    layout.hud_scale = config.hud_scale > 0.05f ? config.hud_scale : 1.0f;
    layout.hud_inset = std::max(0.0f, config.hud_inset);
    return layout;
}

std::uint32_t recommended_internal_scale(const DisplayLayout& layout,
                                         std::uint32_t maximum) noexcept {
    maximum = std::max(1u, maximum);
    const ContentRect content = fit_content_rect(
        layout.output_width, layout.output_height, render_content_aspect(layout));
    if (content.width <= 0 || content.height <= 0) {
        return 1u;
    }
    const auto ceil_ratio = [](int pixels, std::uint32_t guest_pixels) {
        const std::uint64_t positive = static_cast<std::uint64_t>(std::max(1, pixels));
        return static_cast<std::uint32_t>(
            (positive + static_cast<std::uint64_t>(guest_pixels) - 1u) / guest_pixels);
    };
    const std::uint32_t horizontal = ceil_ratio(content.width, kGuestRasterWidth);
    const std::uint32_t vertical = ceil_ratio(content.height, kGuestRasterHeight);
    return std::clamp(std::max(horizontal, vertical), 1u, maximum);
}

HudAffine hud_affine_for_anchor(HudAnchor anchor, const DisplayLayout& layout) noexcept {
    // Everything here is in the 640x480 virtual canvas, which covers the whole
    // output. The transform for a group anchored at canvas position `anchor` is
    //
    //     x' = anchor + s * (x - anchor)
    //
    // where s is the same Hor+ factor the 3D projection gets. That is what
    // makes 2D art square again once presentation stretches the guest
    // framebuffer to the target aspect, and it is why the Center anchor is
    // exactly the transform a world-projected element needs: with anchor 320,
    // x' = 320 + s * (x - 320) is the canvas form of scaling clip x by s.
    //
    // s comes from layout.aspect, not from output_width / output_height. The
    // two can disagree - a forced --aspect, or a render size that is not the
    // window size - and the 3D projection follows layout.aspect, so the 2D has
    // to as well or the HUD is counter-scaled by a different amount than the
    // scene it sits on.
    HudAffine affine;
    // Cover is the lifted renderer's; the legacy renderer keeps stretching a background as before.
    if (anchor == HudAnchor::Fill || anchor == HudAnchor::Cover || layout_is_native(layout)) {
        return affine;
    }

    const ContentRect content = fit_content_rect(
        layout.output_width, layout.output_height, render_content_aspect(layout));
    const float out_w = static_cast<float>(content.width);
    const float out_h = static_cast<float>(content.height);
    // Hor+ never narrows, so a target narrower than 4:3 is pillarboxed by the
    // presenter and the canvas stays untouched - matching render_content_aspect.
    const float scale = horplus_m00_scale(render_content_aspect(layout));
    const float hud = layout.hud_scale;
    const float sx = scale * hud;
    const float sy = hud;
    // hud_inset is in PRESENTED CONTENT pixels; the canvas is what the anchors
    // live in. Using the whole swapchain here under-applied the inset whenever
    // a forced aspect letterboxed or pillarboxed the content.
    const float inset_x = out_w > 0.0f ? layout.hud_inset * (kVirtualHudWidth / out_w) : 0.0f;
    const float inset_y = out_h > 0.0f ? layout.hud_inset * (kVirtualHudHeight / out_h) : 0.0f;

    // A centred 4:3 safe area is the Center anchor: the canvas keeps its own
    // proportions and sits in the middle of a wider output. Menus authored
    // across the whole 4:3 frame use it, which is why nothing needs a separate
    // pillarbox path.
    const int horiz =
        (anchor == HudAnchor::TopLeft || anchor == HudAnchor::Left ||
         anchor == HudAnchor::BottomLeft)
            ? 0
            : (anchor == HudAnchor::TopRight || anchor == HudAnchor::Right ||
               anchor == HudAnchor::BottomRight)
                  ? 2
                  : 1;
    const int vert =
        (anchor == HudAnchor::TopLeft || anchor == HudAnchor::Top ||
         anchor == HudAnchor::TopRight)
            ? 0
            : (anchor == HudAnchor::BottomLeft || anchor == HudAnchor::Bottom ||
               anchor == HudAnchor::BottomRight)
                  ? 2
                  : 1;

    affine.ax = sx;
    if (horiz == 0) {
        affine.bx = inset_x;
    } else if (horiz == 2) {
        affine.bx = kVirtualHudWidth - inset_x - kVirtualHudWidth * sx;
    } else {
        affine.bx = kVirtualHudWidth * 0.5f * (1.0f - sx);
    }

    affine.ay = sy;
    if (vert == 0) {
        affine.by = inset_y;
    } else if (vert == 2) {
        affine.by = kVirtualHudHeight - inset_y - kVirtualHudHeight * sy;
    } else {
        affine.by = kVirtualHudHeight * 0.5f * (1.0f - sy);
    }
    return affine;
}

float apply_hud_x(const HudAffine& affine, float x) noexcept {
    return affine.ax * x + affine.bx;
}

float apply_hud_y(const HudAffine& affine, float y) noexcept {
    return affine.ay * y + affine.by;
}

ContentRect fit_content_rect(int surface_width, int surface_height,
                             float content_aspect) noexcept {
    ContentRect rect;
    if (surface_width <= 0 || surface_height <= 0) {
        return rect;
    }
    if (!(content_aspect > 0.1f) || !std::isfinite(content_aspect)) {
        content_aspect = static_cast<float>(surface_width) / static_cast<float>(surface_height);
    }
    const float surface_aspect =
        static_cast<float>(surface_width) / static_cast<float>(surface_height);
    if (content_aspect > surface_aspect) {
        rect.width = surface_width;
        rect.height = std::max(1, static_cast<int>(std::lround(static_cast<float>(surface_width) /
                                                               content_aspect)));
    } else {
        rect.height = surface_height;
        rect.width = std::max(1, static_cast<int>(std::lround(static_cast<float>(surface_height) *
                                                              content_aspect)));
    }
    rect.x = (surface_width - rect.width) / 2;
    rect.y = (surface_height - rect.height) / 2;
    return rect;
}

void scale_rgba_bilinear(const std::uint32_t* source, int source_width, int source_height,
                         std::uint32_t* dest, int dest_stride, int dest_x, int dest_y,
                         int dest_width, int dest_height) {
    if (source == nullptr || dest == nullptr || source_width <= 0 || source_height <= 0 ||
        dest_width <= 0 || dest_height <= 0 || dest_stride <= 0) {
        return;
    }
    if (source_width == dest_width && source_height == dest_height) {
        for (int y = 0; y < dest_height; ++y) {
            std::memcpy(dest + (dest_y + y) * dest_stride + dest_x,
                        source + y * source_width,
                        static_cast<std::size_t>(dest_width) * sizeof(std::uint32_t));
        }
        return;
    }

    const float x_scale = static_cast<float>(source_width) / static_cast<float>(dest_width);
    const float y_scale = static_cast<float>(source_height) / static_cast<float>(dest_height);

    // The horizontal terms depend only on the destination column, so they are
    // the same for every row. Computing them once turns a per-pixel floor/clamp
    // into a per-pixel table read. The float expressions are unchanged, so the
    // result stays bit-identical to the per-pixel form.
    struct Tap {
        int x0;
        int x1;
        float tx;
    };
    std::vector<Tap> taps(static_cast<std::size_t>(dest_width));
    for (int x = 0; x < dest_width; ++x) {
        const float src_x = (static_cast<float>(x) + 0.5f) * x_scale - 0.5f;
        const int x0 = std::clamp(static_cast<int>(std::floor(src_x)), 0, source_width - 1);
        taps[static_cast<std::size_t>(x)] = Tap{
            x0, std::min(x0 + 1, source_width - 1),
            std::clamp(src_x - static_cast<float>(x0), 0.0f, 1.0f)};
    }

    const auto scale_row = [&](int y) {
        const float src_y = (static_cast<float>(y) + 0.5f) * y_scale - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(src_y)), 0, source_height - 1);
        const int y1 = std::min(y0 + 1, source_height - 1);
        const float ty = std::clamp(src_y - static_cast<float>(y0), 0.0f, 1.0f);
        const std::uint32_t* top_row = source + static_cast<std::size_t>(y0) * source_width;
        const std::uint32_t* bottom_row = source + static_cast<std::size_t>(y1) * source_width;
        std::uint32_t* row = dest + static_cast<std::size_t>(dest_y + y) * dest_stride + dest_x;
        for (int x = 0; x < dest_width; ++x) {
            const Tap tap = taps[static_cast<std::size_t>(x)];
            const std::uint32_t a_pixel = top_row[tap.x0];
            const std::uint32_t b_pixel = top_row[tap.x1];
            const std::uint32_t c_pixel = bottom_row[tap.x0];
            const std::uint32_t d_pixel = bottom_row[tap.x1];
            const auto channel = [](std::uint32_t pixel, int shift) {
                return static_cast<float>((pixel >> shift) & 0xffu);
            };
            const auto blend = [&](int shift) {
                const float a = channel(a_pixel, shift);
                const float b = channel(b_pixel, shift);
                const float c = channel(c_pixel, shift);
                const float d = channel(d_pixel, shift);
                const float top = a + (b - a) * tap.tx;
                const float bottom = c + (d - c) * tap.tx;
                return static_cast<std::uint32_t>(
                    std::clamp(top + (bottom - top) * ty, 0.0f, 255.0f) + 0.5f);
            };
            row[x] = (blend(24) << 24) | (blend(16) << 16) | (blend(8) << 8) | blend(0);
        }
    };

    // Rows are independent, so the result does not depend on the split. The
    // threshold matches the 1:1 staging path in PlumePresenter::present.
    if (dest_height >= 64) {
        host_parallel_for(JobSite::PresentScale, 0, dest_height, scale_row);
    } else {
        for (int y = 0; y < dest_height; ++y) scale_row(y);
    }
}

AspectMode parse_aspect_mode(const std::string& text, bool* ok) {
    const std::string lowered = lower_copy(trim_copy(text));
    const auto set_ok = [&](bool value) {
        if (ok != nullptr) {
            *ok = value;
        }
    };
    if (lowered == "auto") {
        set_ok(true);
        return AspectMode::Auto;
    }
    if (lowered == "4:3" || lowered == "4x3" || lowered == "1.33" || lowered == "native") {
        set_ok(true);
        return AspectMode::FourThree;
    }
    if (lowered == "16:9" || lowered == "16x9" || lowered == "1.77" || lowered == "1.78") {
        set_ok(true);
        return AspectMode::SixteenNine;
    }
    if (lowered == "16:10" || lowered == "16x10" || lowered == "1.6" || lowered == "1.60") {
        set_ok(true);
        return AspectMode::SixteenTen;
    }
    if (lowered == "21:9" || lowered == "21x9" || lowered == "2.33" || lowered == "2.37") {
        set_ok(true);
        return AspectMode::TwentyOneNine;
    }
    if (lowered == "32:9" || lowered == "32x9" || lowered == "3.55" || lowered == "3.56") {
        set_ok(true);
        return AspectMode::ThirtyTwoNine;
    }
    if (lowered == "custom") {
        set_ok(true);
        return AspectMode::Custom;
    }
    set_ok(false);
    return AspectMode::Auto;
}

const char* aspect_mode_name(AspectMode mode) noexcept {
    switch (mode) {
        case AspectMode::FourThree:
            return "4:3";
        case AspectMode::SixteenNine:
            return "16:9";
        case AspectMode::SixteenTen:
            return "16:10";
        case AspectMode::TwentyOneNine:
            return "21:9";
        case AspectMode::ThirtyTwoNine:
            return "32:9";
        case AspectMode::Custom:
            return "custom";
        case AspectMode::Auto:
        default:
            return "auto";
    }
}

bool parse_display_file(const std::string& text, DisplayConfig& config, std::string* error) {
    std::istringstream stream(text);
    std::string line;
    int line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        const auto comment = line.find('#');
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        line = trim_copy(line);
        if (line.empty()) {
            continue;
        }
        const auto split = line.find('=');
        if (split == std::string::npos) {
            if (error != nullptr) {
                *error = "display.cfg line " + std::to_string(line_number) +
                         " is not key=value";
            }
            return false;
        }
        const std::string key = lower_copy(trim_copy(line.substr(0, split)));
        const std::string value = trim_copy(line.substr(split + 1));
        const auto fail = [&](const char* message) {
            if (error != nullptr) {
                *error = std::string("display.cfg line ") + std::to_string(line_number) + ": " +
                         message;
            }
            return false;
        };
        if (key == "width" || key == "window_width") {
            if (!parse_int(value, &config.window_width) || config.window_width < 160) {
                return fail("invalid width");
            }
        } else if (key == "height" || key == "window_height") {
            if (!parse_int(value, &config.window_height) || config.window_height < 120) {
                return fail("invalid height");
            }
        } else if (key == "fullscreen") {
            if (!parse_bool(value, &config.fullscreen)) {
                return fail("invalid fullscreen");
            }
        } else if (key == "render_width") {
            if (!parse_int(value, &config.render_width) || config.render_width < 0) {
                return fail("invalid render_width");
            }
        } else if (key == "render_height") {
            if (!parse_int(value, &config.render_height) || config.render_height < 0) {
                return fail("invalid render_height");
            }
        } else if (key == "aspect") {
            bool ok = false;
            config.aspect = parse_aspect_mode(value, &ok);
            if (!ok) {
                return fail("invalid aspect");
            }
        } else if (key == "aspect_ratio" || key == "custom_aspect") {
            if (!parse_float(value, &config.custom_aspect) || config.custom_aspect <= 0.1f) {
                return fail("invalid aspect_ratio");
            }
            if (config.aspect == AspectMode::Auto) {
                config.aspect = AspectMode::Custom;
            }
        } else if (key == "hud_scale") {
            if (!parse_float(value, &config.hud_scale) || config.hud_scale <= 0.05f) {
                return fail("invalid hud_scale");
            }
        } else if (key == "hud_inset" || key == "hud_safe_zone") {
            if (!parse_float(value, &config.hud_inset) || config.hud_inset < 0.0f) {
                return fail("invalid hud_inset");
            }
        } else if (key == "render_rate") {
            // The same range as --render-rate: unlocked (0) or a rate in (0, 1000] Hz.
            char* end = nullptr;
            const double rate = std::strtod(value.c_str(), &end);
            if (lower_copy(value) == "unlocked") {
                config.render_rate = 0.0;
            } else if (!value.empty() && end != value.c_str() && *end == '\0' && std::isfinite(rate) &&
                       rate > 0.0 && rate <= 1000.0) {
                config.render_rate = rate;
            } else {
                return fail("invalid render_rate (unlocked or a value in (0,1000])");
            }
        } else if (key == "vsync") {
            bool on = false;
            if (!parse_bool(value, &on)) {
                return fail("invalid vsync");
            }
            config.vsync = on;
        } else if (key == "frame_interpolation") {
            bool on = false;
            if (!parse_bool(value, &on)) {
                return fail("invalid frame_interpolation");
            }
            config.frame_interpolation = on;
        } else if (key == "draw_distance") {
            float scale = 0.0f;
            if (!parse_float(value, &scale) || !(scale >= 1.0f && scale <= 8.0f)) {
                return fail("invalid draw_distance (1 = the original game .. 8)");
            }
            config.draw_distance = scale;
        } else if (key == "fps_counter") {
            bool on = false;
            if (!parse_bool(value, &on)) {
                return fail("invalid fps_counter");
            }
            config.fps_counter = on;
        } else {
            return fail("unknown key");
        }
    }
    return true;
}

std::string format_display_config(const DisplayConfig& config) {
    std::ostringstream out;
    out << "window " << config.window_width << "x" << config.window_height
        << (config.fullscreen ? " fullscreen" : " windowed") << ", aspect "
        << aspect_mode_name(config.aspect);
    if (config.aspect == AspectMode::Custom) {
        out << " (" << config.custom_aspect << ")";
    }
    if (config.render_width > 0 && config.render_height > 0) {
        out << ", render " << config.render_width << "x" << config.render_height;
    }
    out << ", HUD scale " << config.hud_scale << ", inset " << config.hud_inset;
    return out.str();
}

}  // namespace gekko
