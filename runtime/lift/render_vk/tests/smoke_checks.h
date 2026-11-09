#pragma once

// The Vulkan renderer's headless checks, shared by the Windows test (vk_smoke.cpp) and the Android activity
// (android/main.cpp): packets of three views - two side by side and a full-screen overlay drawn over them (split
// screen), in a centred 1:1 content box - rendered through the clear, the overlay's start from the views under it, the
// multisampled resolve and the compose, read back and compared with the values the chain may give, and rendered a
// second time to the same bytes.

#include <cstdint>
#include <functional>
#include <string>

#include "render/frame.h"
#include "render_vk/renderer.h"

namespace shadow::render_vk {

// A split-screen packet: two views side by side and an overlay over both, one scene pass each with a draw of a mesh
// the renderer does not have (counted as skipped), and a history save on the overlay.
render::FramePacket split_screen_packet(render::FrameBuilder& builder, std::uint64_t tick,
                                        const render::Float4& background, float content_aspect);

// The number of failed checks (0: all passed); every result line goes to `log`.
int run_smoke_checks(Renderer& renderer, const std::function<void(const std::string&)>& log);

}  // namespace shadow::render_vk
