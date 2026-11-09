#pragma once

// What the device can do, in the terms of docs/VULKAN_RENDERER.md 5-6, as lines of text (the Windows tests print them;
// on Android they go to logcat). Recorded per device in the design doc: the evidence for the format, texture and memory
// decisions a phone needs.

#include <string>
#include <vector>

#include "render_vk/device.h"

namespace shadow::render_vk {

std::vector<std::string> capability_report(const Device& device);

}  // namespace shadow::render_vk
