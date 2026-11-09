#include "gekko/render_geometry.h"

#include <cmath>

namespace gekko {

PresentGeometry fit_present_geometry(int surface_width, int surface_height,
                                     float content_aspect) noexcept {
    PresentGeometry geometry;
    geometry.surface_width = surface_width;
    geometry.surface_height = surface_height;
    if (surface_width <= 0 || surface_height <= 0) {
        return geometry;
    }
    if (!(content_aspect > 0.1f) || !std::isfinite(content_aspect)) {
        content_aspect = static_cast<float>(surface_width) / static_cast<float>(surface_height);
    }
    const float surface_aspect =
        static_cast<float>(surface_width) / static_cast<float>(surface_height);
    if (content_aspect > surface_aspect) {
        // Letterbox: full width, bars top and bottom.
        geometry.content.width = surface_width;
        geometry.content.height = std::max(
            1, static_cast<int>(std::lround(static_cast<float>(surface_width) / content_aspect)));
        geometry.content.height = std::min(geometry.content.height, surface_height);
    } else {
        // Pillarbox: full height, bars left and right.
        geometry.content.height = surface_height;
        geometry.content.width = std::max(
            1, static_cast<int>(std::lround(static_cast<float>(surface_height) * content_aspect)));
        geometry.content.width = std::min(geometry.content.width, surface_width);
    }
    // Integer division floors, so when the leftover is odd the left/top bar is
    // the narrow one. That is a deliberate, stated choice rather than an
    // accident of the expression: the caller clears `bar_left` and `bar_right`
    // independently, so an asymmetric split is covered exactly.
    geometry.content.x = (surface_width - geometry.content.width) / 2;
    geometry.content.y = (surface_height - geometry.content.height) / 2;
    return geometry;
}

}  // namespace gekko
