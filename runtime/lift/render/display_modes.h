#pragma once

// The displays this PC has and the resolution families a player chooses from (docs/RESOLUTION_AUDIT.md).
//
// enumerate_displays asks DXGI for every adapter output (its desktop rectangle and the modes it supports) and the
// display driver for what the desktop really is (EnumDisplaySettings: its size and refresh, physical pixels whatever
// the process's DPI awareness) and the monitor's name (EnumDisplayDevices). Nothing is set on any display.
//
// The presets are the established PC display resolutions, grouped by their commercial aspect family. A family is a
// name, not a ratio: 3440x1440 is "21:9" although it is 2.389:1, and every rendering calculation uses the actual
// width / height. aspect_family classifies any size the same way, so detected modes and custom sizes are named
// consistently; `exact` says whether the size's pixels are exactly the family's nominal ratio (1366x768 is a near-16:9).

#include <cstdint>
#include <string>
#include <vector>

namespace shadow::render {

struct DisplayMode {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t refresh_numerator = 0;  // refresh rate = numerator / denominator Hz (DXGI_RATIONAL)
    std::uint32_t refresh_denominator = 1;
};

struct DisplayOutput {
    std::string adapter;      // the adapter's description
    std::string device;       // the output's GDI device name (\\.\DISPLAY1)
    std::string monitor;      // the monitor's name as the driver reports it
    bool primary = false;     // the primary display (desktop origin)
    std::int32_t desktop_left = 0, desktop_top = 0;
    std::uint32_t desktop_width = 0, desktop_height = 0;  // the desktop's physical size: the native mode in use
    std::uint32_t desktop_refresh_hz = 0;
    std::vector<DisplayMode> modes;  // supported modes (R8G8B8A8_UNORM, progressive), widest refresh last
};

// Every output of every adapter; empty (with the reason) when DXGI cannot be asked.
std::vector<DisplayOutput> enumerate_displays(std::string* error);

struct AspectFamily {
    const char* name;  // "16:9", "16:10", "21:9", "32:10", "32:9", "4:3", "5:4", or "other"
    bool exact;        // width / height is exactly the family's nominal ratio
};
AspectFamily aspect_family(std::uint32_t width, std::uint32_t height) noexcept;

struct ResolutionPreset {
    std::uint32_t width;
    std::uint32_t height;
    const char* family;
};
// The recognized resolution families (not the renderer's limits: any size up to 16384 renders natively).
const std::vector<ResolutionPreset>& resolution_presets();

// The JSON `--list-displays` prints: outputs, their modes and families, and the presets.
std::string displays_json(const std::vector<DisplayOutput>& outputs, const std::string& error);

}  // namespace shadow::render
