#include "render/display_modes.h"

#include <windows.h>  // NOMINMAX and WIN32_LEAN_AND_MEAN come from the target
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <tuple>

namespace shadow::render {

namespace {

std::string narrow(const wchar_t* text) {
    std::string out;
    for (const wchar_t* c = text; c != nullptr && *c != L'\0'; ++c)
        out.push_back(*c >= 0x20 && *c < 0x7F ? static_cast<char>(*c) : '?');  // json_text escapes quotes and backslashes
    return out;
}

std::string json_text(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

struct Nominal {
    const char* name;
    std::uint32_t w, h;  // the nominal ratio
};
constexpr Nominal kFamilies[] = {{"4:3", 4, 3},   {"5:4", 5, 4},   {"16:10", 16, 10}, {"16:9", 16, 9},
                                 {"21:9", 21, 9}, {"32:10", 32, 10}, {"32:9", 32, 9}};

}  // namespace

AspectFamily aspect_family(std::uint32_t width, std::uint32_t height) noexcept {
    if (width == 0u || height == 0u)
        return {"other", false};
    const double ratio = static_cast<double>(width) / height;
    // The 21:9 family is a commercial name for 2.37:1 (2560x1080, 5120x2160), 2.389:1 (3440x1440) and 2.4:1
    // (3840x1600) panels: a band, not a ratio.
    if (ratio >= 2.30 && ratio <= 2.45)
        return {"21:9", static_cast<std::uint64_t>(width) * 9u == static_cast<std::uint64_t>(height) * 21u};
    const Nominal* best = nullptr;
    double best_error = 0.0;
    for (const Nominal& f : kFamilies) {
        const double error = std::fabs(ratio / (static_cast<double>(f.w) / f.h) - 1.0);
        if (best == nullptr || error < best_error) {
            best = &f;
            best_error = error;
        }
    }
    // Within 1% is that family (1366x768 and 1360x768 are near-16:9); anything else is its own shape.
    if (best == nullptr || best_error > 0.01)
        return {"other", false};
    return {best->name, static_cast<std::uint64_t>(width) * best->h == static_cast<std::uint64_t>(height) * best->w};
}

const std::vector<ResolutionPreset>& resolution_presets() {
    static const std::vector<ResolutionPreset> kPresets = {
        // 16:9 (and the near-16:9 1366x768 most laptops report)
        {1280, 720, "16:9"}, {1366, 768, "16:9"}, {1600, 900, "16:9"}, {1920, 1080, "16:9"}, {2560, 1440, "16:9"},
        {3200, 1800, "16:9"}, {3840, 2160, "16:9"}, {5120, 2880, "16:9"}, {7680, 4320, "16:9"},
        // 16:10
        {1280, 800, "16:10"}, {1440, 900, "16:10"}, {1680, 1050, "16:10"}, {1920, 1200, "16:10"},
        {2560, 1600, "16:10"}, {2880, 1800, "16:10"}, {3840, 2400, "16:10"}, {5120, 3200, "16:10"},
        // 21:9 (ultrawide; each panel's own ratio is used for rendering)
        {2560, 1080, "21:9"}, {3440, 1440, "21:9"}, {3840, 1600, "21:9"}, {5120, 2160, "21:9"},
        // 32:10 and 32:9 (super ultrawide)
        {3840, 1200, "32:10"}, {5120, 1600, "32:10"}, {3840, 1080, "32:9"}, {5120, 1440, "32:9"},
        {7680, 2160, "32:9"},
        // 4:3 and 5:4 (legacy)
        {640, 480, "4:3"}, {800, 600, "4:3"}, {1024, 768, "4:3"}, {1280, 960, "4:3"}, {1600, 1200, "4:3"},
        {1280, 1024, "5:4"},
    };
    return kPresets;
}

std::vector<DisplayOutput> enumerate_displays(std::string* error) {
    using Microsoft::WRL::ComPtr;
    std::vector<DisplayOutput> outputs;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        if (error != nullptr)
            *error = "CreateDXGIFactory1 failed";
        return outputs;
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 adapter_desc{};
        adapter->GetDesc1(&adapter_desc);
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)))
                continue;
            DisplayOutput out;
            out.adapter = narrow(adapter_desc.Description);
            out.device = narrow(desc.DeviceName);
            out.desktop_left = desc.DesktopCoordinates.left;
            out.desktop_top = desc.DesktopCoordinates.top;
            out.desktop_width = static_cast<std::uint32_t>(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left);
            out.desktop_height = static_cast<std::uint32_t>(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
            out.primary = desc.DesktopCoordinates.left == 0 && desc.DesktopCoordinates.top == 0;
            // The desktop as the driver has it (physical pixels whatever this process's DPI awareness) and its refresh.
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
                out.desktop_width = mode.dmPelsWidth;
                out.desktop_height = mode.dmPelsHeight;
                out.desktop_refresh_hz = mode.dmDisplayFrequency;
            }
            DISPLAY_DEVICEW monitor{};
            monitor.cb = sizeof(monitor);
            if (EnumDisplayDevicesW(desc.DeviceName, 0, &monitor, 0))
                out.monitor = narrow(monitor.DeviceString);
            UINT count = 0;
            const DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
            if (SUCCEEDED(output->GetDisplayModeList(format, 0, &count, nullptr)) && count != 0u) {
                std::vector<DXGI_MODE_DESC> modes(count);
                if (SUCCEEDED(output->GetDisplayModeList(format, 0, &count, modes.data()))) {
                    std::set<std::tuple<UINT, UINT, UINT, UINT>> seen;
                    for (UINT m = 0; m < count; ++m) {
                        const DXGI_MODE_DESC& d = modes[m];
                        if (d.ScanlineOrdering != DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE &&
                            d.ScanlineOrdering != DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED)
                            continue;
                        if (!seen.insert({d.Width, d.Height, d.RefreshRate.Numerator, d.RefreshRate.Denominator}).second)
                            continue;
                        out.modes.push_back({d.Width, d.Height, d.RefreshRate.Numerator,
                                             d.RefreshRate.Denominator == 0u ? 1u : d.RefreshRate.Denominator});
                    }
                }
            }
            outputs.push_back(std::move(out));
            output.Reset();
        }
        adapter.Reset();
    }
    if (outputs.empty() && error != nullptr)
        *error = "no display output found";
    return outputs;
}

std::string displays_json(const std::vector<DisplayOutput>& outputs, const std::string& error) {
    std::string json = "{\n  \"schema\": 1,\n  \"error\": \"" + json_text(error) + "\",\n  \"outputs\": [";
    char buffer[256];
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const DisplayOutput& o = outputs[i];
        const AspectFamily family = aspect_family(o.desktop_width, o.desktop_height);
        std::snprintf(buffer, sizeof(buffer),
                      "%s\n    {\"device\": \"%s\", \"monitor\": \"%s\", \"adapter\": \"%s\", \"primary\": %s, "
                      "\"desktop\": {\"left\": %d, \"top\": %d, \"width\": %u, \"height\": %u, \"refresh_hz\": %u, "
                      "\"family\": \"%s\", \"exact\": %s},\n     \"modes\": [",
                      i == 0 ? "" : ",", json_text(o.device).c_str(), json_text(o.monitor).c_str(),
                      json_text(o.adapter).c_str(), o.primary ? "true" : "false", o.desktop_left, o.desktop_top,
                      o.desktop_width, o.desktop_height, o.desktop_refresh_hz, family.name,
                      family.exact ? "true" : "false");
        json += buffer;
        for (std::size_t m = 0; m < o.modes.size(); ++m) {
            const DisplayMode& d = o.modes[m];
            std::snprintf(buffer, sizeof(buffer), "%s{\"width\": %u, \"height\": %u, \"refresh_hz\": %.3f, \"family\": \"%s\"}",
                          m == 0 ? "" : ", ", d.width, d.height,
                          static_cast<double>(d.refresh_numerator) / d.refresh_denominator,
                          aspect_family(d.width, d.height).name);
            json += buffer;
        }
        json += "]}";
    }
    json += "\n  ],\n  \"presets\": [";
    const std::vector<ResolutionPreset>& presets = resolution_presets();
    for (std::size_t p = 0; p < presets.size(); ++p) {
        std::snprintf(buffer, sizeof(buffer), "%s{\"width\": %u, \"height\": %u, \"family\": \"%s\"}",
                      p == 0 ? "" : ", ", presets[p].width, presets[p].height, presets[p].family);
        json += buffer;
    }
    json += "]\n}\n";
    return json;
}

}  // namespace shadow::render
