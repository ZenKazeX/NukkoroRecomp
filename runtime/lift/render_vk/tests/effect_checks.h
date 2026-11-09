#pragma once

// The Vulkan renderer's effect checks (stage V4), shared by the Windows test and the Android activity: packets whose
// right pixels follow from the packet alone -
//   frame 1: a green canvas quad, then the view's history saved;
//   frame 2: a red canvas quad, a skinned quad moved by its bone, a delta-morphed quad moved by its target; a snapshot;
//            a quad sampling the snapshot where the red quad is (a copy of it elsewhere), a quad sampling the history
//            where frame 1's green quad was; glare sources (a white quad) and the glare; a canvas quad after the glare
//            (the composite folds into the resolve, B3a) -
// checked at interior points, frame 2 rendered with the fold and without it (the pictures compared), rendered twice
// (identical), then the history invalidated (its draw left out) and a capture that does not commit (no history made).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "render_vk/renderer.h"

namespace shadow::render_vk {

inline constexpr std::uint32_t kEffectCheckWidth = 160;
inline constexpr std::uint32_t kEffectCheckHeight = 120;
// The number of failed checks (0: all passed); every result line goes to `log`. `rgba` (when not null) receives frame
// 2's picture (kEffectCheckWidth x kEffectCheckHeight, RGBA8, rows top first).
int run_effect_checks(Renderer& renderer, const std::function<void(const std::string&)>& log,
                      std::vector<std::uint8_t>* rgba = nullptr);

}  // namespace shadow::render_vk
