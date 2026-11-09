#pragma once

// The Options menu's Display page, drawn (gekko/native_options_display.h): the rows the game thread published, in the
// front-end's own glyph sheets, on the menu's 640 x 480 canvas kept 4:3 and centred like the menu itself. Each row is
// "label  value" in the page's text slots (fontnull text1..text7: x 399, y 140 + 40 n). The row with the cursor is the
// game's selected yellow, its value as "< value >" (the original marks a selection only by that yellow: no bar).
// Text is drawn as the game's font draws it (0x8004A09C): a shadow pass down-right that darkens by the glyph's colour,
// then the glyph added onto the picture.

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "render/frame.h"

namespace shadow::render {
class Renderer;
}

namespace gekko {

class OptionsDisplayOverlay {
public:
    // UI thread, right before the packet is rendered: appends the page's view while the game shows the page.
    void append(shadow::render::FramePacket& packet, shadow::render::Renderer& renderer);

private:
    struct Run {
        std::uint32_t sheet;
        std::vector<shadow::render::ImmediateVertex> shadow, text;  // the game's two passes
    };
    float width(const std::u16string& text, float scale) const;
    void text(const std::u16string& text, float x, float y, float scale, std::uint32_t colour);
    void glyph(char16_t code, float& x, float y, float scale, std::uint32_t colour);

    std::uint32_t font_serial_ = 0;
    std::vector<shadow::render::TextureHandle> sheets_;
    std::vector<std::uint32_t> sheet_sizes_;  // width, height pairs
    std::vector<Run> runs_;
    const void* font_ = nullptr;  // the shared font while append() holds its lock
};

}  // namespace gekko
