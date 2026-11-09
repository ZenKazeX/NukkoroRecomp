#pragma once

// The frame graph's checks (stage V2), shared by the Windows test and the Android activity:
//   the graph on its own (render_vk/frame_graph.h) - a frame of clears and copies whose plan is known: the pass nothing
//   reads culled, a texture taking over an earlier one's image, the barriers (a discard from UNDEFINED, a layout change,
//   none for a read after a read), the load and store ops, the frame executed and read back to the exact pixels, the
//   second frame making no new image; and the invalid graphs it must refuse (a read before any write, an add before any
//   write, a texture read and written by one pass, a depth access of a colour texture, a clear that is not an
//   attachment's);
//   the graph in the renderer - glare sources no glare reads and a snapshot nobody samples culled with the picture
//   unchanged, the pool making nothing new for a frame it has seen, split-screen views sharing their scene targets.

#include <functional>
#include <string>

#include "render_vk/renderer.h"

namespace shadow::render_vk {

// The number of failed checks (0: all passed); every result line goes to `log`.
int run_graph_checks(Renderer& renderer, const std::function<void(const std::string&)>& log);

}  // namespace shadow::render_vk
