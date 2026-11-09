#pragma once

// Host display layout for the native port.
//
// LEGACY RENDERER ONLY (NATIVE_AUDIT.md C15, lies list 15): the legacy GX/EFB replay renderer models the
// GameCube's 640x528 EFB so guest EFB copies, scissor units and its DX12 rasterizer keep the console's
// coordinates. The lifted R2 renderer has no EFB (G5 removes the legacy renderer from the lifted build). Presentation fills the
// window from that framebuffer. 2D/HUD draws are remapped in a 640x480 virtual
// space with per-element anchors; HUD scale is independent of aspect
// expansion. Perspective draws receive the same Hor+ scale as the 2D canvas;
// measured routes confirm that RenderWare submits enough side geometry for the
// widened host clipper rather than leaving black gutters.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gekko/camera.h"
#include "gekko/render_geometry.h"
#include "ui/hud_layout.h"

namespace gekko {

// Aliases of render_geometry.h. kNativeAspect is the DISPLAY aspect the
// original composes for, not the game's projection aspect (1.3195877).
constexpr float kNativeAspect = kReferenceAspect;
constexpr float kVirtualHudWidth = kHudCanvasWidth;
constexpr float kVirtualHudHeight = kHudCanvasHeight;

enum class AspectMode : std::uint8_t {
    Auto = 0,
    FourThree,
    SixteenNine,
    SixteenTen,
    TwentyOneNine,
    ThirtyTwoNine,
    Custom,
};

// NE-RW2c: the 2D layout policy's anchors, bounds and classifiers are runtime/lift/ui/hud_layout.h (plain values);
// these names stay for the legacy renderer and its tests.
using HudAnchor = shadow::ui::HudAnchor;
using HudBounds = shadow::ui::HudBounds;
using shadow::ui::classify_group_anchor;
using shadow::ui::classify_hud_anchor;
using shadow::ui::spans_canvas_height;
using shadow::ui::spans_canvas_width;

struct DisplayConfig {
    int window_width = 640;
    int window_height = 480;
    bool fullscreen = false;
    bool exclusive = false;  // fullscreen as the monitor's display mode fullscreen_width x fullscreen_height
    // The exclusive display mode (fullscreen_width / fullscreen_height); 0 x 0: the desktop's own mode.
    int fullscreen_width = 0;
    int fullscreen_height = 0;
    // The legacy renderer's render size. The lifted build ignores it (with a warning): the picture follows the window
    // or screen and the resolution scale.
    int render_width = 0;
    int render_height = 0;
    std::optional<double> aspect_ratio;  // the screen ratio exactly (0 = Auto); the enum below is the legacy view
    std::optional<double> render_scale;  // the resolution scale: percent per side of the picture (25..200)
    // Values display.cfg held that this build accepts only changed (an older build's render_scale=400 is 200 now):
    // said once at startup; the file is not rewritten for them.
    std::vector<std::string> warnings;
    std::optional<int> msaa;             // the scene's MSAA samples: 1 (off), 2, 4, 8, 16, 32
    std::optional<int> anisotropy;       // the game's filtered textures: 1 (off), 2, 4, 8, 16
    std::optional<double> gameplay_aspect;  // the game logic's aspect: 0 follow, 4/3 original, a ratio
    AspectMode aspect = AspectMode::Auto;
    float custom_aspect = 16.0f / 9.0f;
    float hud_scale = 1.0f;
    float hud_inset = 0.0f;
    // The presentation keys (display.cfg only, the launcher's flags win over them): unset = the flag's default.
    // render_rate 0 = unlocked (--render-rate), vsync (--vsync / --no-vsync), frame_interpolation
    // (--frame-interpolation), draw_distance 1..8 (--draw-distance), fps_counter (--fps-counter).
    std::optional<double> render_rate;
    std::optional<bool> vsync;
    std::optional<bool> frame_interpolation;
    std::optional<float> draw_distance;
    std::optional<bool> fps_counter;
};

struct DisplayLayout {
    int output_width = 640;
    int output_height = 480;
    float aspect = kNativeAspect;
    float hud_scale = 1.0f;
    float hud_inset = 0.0f;
    // THE camera coverage for this layout. World culling, the native
    // projection, the HUD affine and every world-anchored screen test read
    // this one value.
    //
    // DERIVED, not stored. It was briefly a member assigned in
    // make_display_layout, which is a denormalised field that five
    // construction sites have to remember to set - `resize_layout`,
    // `hud_layout`, `flush_layout_`, NativeHud's `layout_` and the local in
    // apply_output_resize are all default-constructed. Any of them reaching a
    // consumer with the default 1.0 means the PROJECTION never widens while
    // the presenter still fits the content to the target aspect, i.e. the whole
    // 3D image is horizontally stretched. Deriving it from the layout's own
    // aspect makes that state unrepresentable, and it is still exactly one
    // implementation: camera::Coverage::from_aspect.
    camera::Coverage coverage() const noexcept {
        // Hor+ only ever widens, so coverage is taken against the clamped
        // content aspect - the same clamp render_content_aspect() applies.
        const float target = (aspect > 0.1f && std::isfinite(aspect))
                                 ? (aspect > kNativeAspect ? aspect : kNativeAspect)
                                 : kNativeAspect;
        return camera::Coverage::from_aspect(target);
    }
};

struct HudAffine {
    float ax = 1.0f;
    float bx = 0.0f;
    float ay = 1.0f;
    float by = 0.0f;
};

struct ContentRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

float aspect_ratio_for_mode(AspectMode mode, float custom, int output_width,
                            int output_height) noexcept;
float horplus_m00_scale(float target_aspect) noexcept;
// Aspect the 3D scene is rendered for. Hor+ only ever widens: a window
// narrower than the guest's own 4:3 keeps the original frustum and is
// pillarboxed by the presenter, because narrowing the horizontal field of
// view would crop scene the original composition relies on.
float render_content_aspect(const DisplayLayout& layout) noexcept;
bool layout_is_native(const DisplayLayout& layout) noexcept;
DisplayLayout make_display_layout(const DisplayConfig& config, int output_width,
                                  int output_height) noexcept;

// Smallest integer EFB scale that avoids magnifying the native raster along
// either axis of the fitted presentation rectangle. The guest remains
// 640x480/640x528; this selects only the physical backing density. Looking at
// height alone is insufficient for anamorphic widescreen: 1920x1080 needs 3x
// because its horizontal ratio is 1920/640 even though 1080/480 is only 2.25.
std::uint32_t recommended_internal_scale(const DisplayLayout& layout,
                                         std::uint32_t maximum = 6u) noexcept;

HudAffine hud_affine_for_anchor(HudAnchor anchor, const DisplayLayout& layout) noexcept;
float apply_hud_x(const HudAffine& affine, float x) noexcept;
float apply_hud_y(const HudAffine& affine, float y) noexcept;

ContentRect fit_content_rect(int surface_width, int surface_height,
                             float content_aspect) noexcept;

void scale_rgba_bilinear(const std::uint32_t* source, int source_width, int source_height,
                         std::uint32_t* dest, int dest_stride, int dest_x, int dest_y,
                         int dest_width, int dest_height);

AspectMode parse_aspect_mode(const std::string& text, bool* ok = nullptr);
const char* aspect_mode_name(AspectMode mode) noexcept;
bool parse_display_file(const std::string& text, DisplayConfig& config, std::string* error);
std::string format_display_config(const DisplayConfig& config);

}  // namespace gekko
