#pragma once

// The effect layer list drawn over the picture (docs/EFFECTS_INVENTORY.md "Layer switches"): F6 shows it, the keys
// gekko/effect_layer_keys.h names (the numeric keypad) edit render/effect_layers.h's switches. While hidden and a switch is set, one status line
// says what is not drawn. Drawn into the presented packet only, after its captures were taken: captures show the game.

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "render/frame.h"

namespace shadow::render {
class Renderer;
}

namespace gekko {

class EffectLayerOverlay {
public:
    // UI thread: the window's fresh effect keys (gekko/effect_layer_keys.h bits).
    void keys(unsigned keys);
    // UI thread, right before the packet is rendered: appends the overlay view when there is something to show.
    void append(shadow::render::FramePacket& packet, shadow::render::Renderer& renderer, std::uint32_t output_width,
                std::uint32_t output_height);
    // The FPS counter (top right): presented frames per second, mean and worst frame time of each half second.
    void set_fps_counter(bool on) {
        fps_ = on;
        fps_text_.clear();
        fps_frames_ = 0;
        fps_worst_ms_ = 0.0;
        fps_window_start_ = {};
    }

private:
    bool font(shadow::render::Renderer& renderer);
    void text(float x, float y, const std::string& line, std::uint32_t colour);
    void box(float x0, float y0, float x1, float y1, std::uint32_t colour);
    void count_frame();

    bool visible_ = false;
    bool fps_ = false;
    std::string fps_text_;
    std::uint32_t fps_frames_ = 0;
    double fps_worst_ms_ = 0.0;
    std::chrono::steady_clock::time_point fps_window_start_{}, fps_last_{};
    std::uint32_t cursor_ = 1;  // a layer (EffectLayer), never None
    bool font_tried_ = false;
    shadow::render::TextureHandle font_;
    // The atlas: 16 x 6 cells of the printable ASCII from ' ', each kCell wide and kRow high.
    std::vector<shadow::render::ImmediateVertex> glyphs_;
    std::vector<shadow::render::ImmediateVertex> panels_;
};

}  // namespace gekko
