// The watch camera's noise sheet, natively (GUN Fortress's security cameras: WatchCamera::Monitor, fn_80268660).
//
// While a watch camera has the view, the game draws BS0600CAMERANOISE.DFF - a unit quad (corners +-0.5) with a
// scrolling scanline texture (s0501_sa029, 2 x 128, both columns equal) - 1.1 units in front of the eye on the
// camera's own axes, its x and y scaled by 2 x 1.1 x the camera's window: at that distance, exactly the picture's
// edges. The camera's window is the AUTHORED picture's (the camera sync's window step, scaffold projection.cpp
// camera_picture_window), so on a wider view (Hor+) or a narrower one (Vert+) the console's size covered only the
// authored 4:3 part and left the rest of the picture bare. The size here is the same arithmetic on the window the view
// DISPLAYS (render/view.h displayed_window, the window the world is drawn with): the whole picture at every aspect.
// At the authored aspect the displayed window is the camera's own, and the result is the game's, bit for bit. The
// texture is constant across the sheet's width, so a wider sheet shows the same scanlines with no seam.
#pragma once

#include "render/view.h"

namespace shadow::effects {

// The sheet's scale on the camera's x and y axes.
struct SheetScale {
    float x = 1.0f;
    float y = 1.0f;
};

// [r2-0x19B0] (0x805F8DD0): the sheet's distance in front of the eye, along the camera's view.
inline constexpr float kWatchSheetDistance = 1.1f;
// [r2-0x19AC] (0x805F8DD4): a window half-extent to the unit quad's whole extent.
inline constexpr float kWatchSheetSpan = 2.0f;

// fn_80268660's scale of a window, in its order (0x80268714..0x80268728): span x (window x distance), each step one
// single-precision multiply.
inline SheetScale watch_camera_sheet_scale(render::ViewWindow window) noexcept {
    return SheetScale{kWatchSheetSpan * (window.x * kWatchSheetDistance),
                      kWatchSheetSpan * (window.y * kWatchSheetDistance)};
}

}  // namespace shadow::effects
