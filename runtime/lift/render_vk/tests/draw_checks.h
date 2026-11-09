#pragma once

// The Vulkan renderer's draw checks (stage V3), shared by the Windows test and the Android activity: one packet whose
// right pixels are known from the packet alone - canvas quads (placement, an exact colour, a half-transparent blend, a
// texture sampled point by point), world meshes projected with the shared camera math (render/frame_math.h): one quad
// counter-clockwise on screen that must show and one clockwise that the back-face cull must remove (the front-face
// convention through Vulkan's negative-height viewport), three overlapping quads that the depth test must order, and a
// particle - rendered, read back, checked at interior points, and rendered a second time to the same bytes.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "render_vk/renderer.h"

namespace shadow::render_vk {

// The number of failed checks (0: all passed); every result line goes to `log`. `rgba` (when not null) receives the
// rendered picture (kDrawCheckWidth x kDrawCheckHeight, RGBA8, rows top first).
inline constexpr std::uint32_t kDrawCheckWidth = 160;
inline constexpr std::uint32_t kDrawCheckHeight = 120;
int run_draw_checks(Renderer& renderer, const std::function<void(const std::string&)>& log,
                    std::vector<std::uint8_t>* rgba = nullptr);

}  // namespace shadow::render_vk
