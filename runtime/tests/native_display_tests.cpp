#include "native_display.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>

namespace {
int failures = 0;
void check(bool value, const char* message) {
    if (!value) { std::printf("FAIL: %s\n", message); ++failures; }
}
}

int main() {
    shadow::display::State display;
    check(!display.configured() && display.coverage_scale() == 1.0f,
          "unconfigured is distinct from configured 4:3");
    gekko::DisplayConfig config;
    display.configure(config);
    check(display.configured() && display.coverage_scale() == 1.0f, "4:3 identity");
    display.resize(1920, 1080);
    check(display.layout().output_width == 1920 && display.layout().output_height == 1080,
          "native window size ownership");
    check(std::fabs(display.coverage_scale() - 0.75f) < 1e-6f, "widescreen coverage");
    display.resize(0, -1);
    check(display.layout().output_width == 1920 && display.layout().output_height == 1080,
          "minimized dimensions do not replace usable size");
    display.resize(800, 0);
    check(display.layout().output_width == 800 && display.layout().output_height == 1080 &&
          display.coverage_scale() == 1.0f, "partial resize and narrow coverage");
    config.window_width = -1;
    config.window_height = 0;
    display.configure(config);
    check(display.layout().output_width == 640 && display.layout().output_height == 480,
          "invalid startup dimensions use the established composition");
    config.render_width = 2560;
    config.render_height = 1440;
    display.configure(config);
    display.resize(3840, 2160);
    check(display.layout().output_width == 2560 && display.layout().output_height == 1440,
          "explicit layout resolution survives output resizing");
    config.aspect = gekko::AspectMode::Custom;
    config.custom_aspect = std::numeric_limits<float>::quiet_NaN();
    display.configure(config);
    check(std::isfinite(display.coverage_scale()) && display.coverage_scale() > 0,
          "invalid custom aspect cannot poison camera coverage");

    // A window/config writer and two consumers must never observe a mixed
    // config/dimension snapshot. The owner has no renderer or hardware lifetime.
    gekko::DisplayConfig a, b;
    a.window_width = 1280; a.window_height = 720; a.hud_scale = 1.25f;
    b.window_width = 800; b.window_height = 600; b.hud_scale = 2.0f;
    display.configure(a);
    std::atomic<bool> done{false}, invalid{false};
    auto consume = [&] {
        do {
            const auto layout = display.layout();
            const bool first = layout.output_width == 1280 && layout.output_height == 720 &&
                               layout.hud_scale == 1.25f;
            const bool second = layout.output_width == 800 && layout.output_height == 600 &&
                                layout.hud_scale == 2.0f;
            const float scale = display.coverage_scale();
            if ((!first && !second) || !display.configured() ||
                (scale != 1.0f && std::fabs(scale - 0.75f) > 1e-6f)) invalid = true;
        } while (!done.load());
    };
    std::thread reader0(consume), reader1(consume);
    for (int i = 0; i < 10000; ++i) display.configure(i % 2 ? a : b);
    done = true;
    reader0.join(); reader1.join();
    check(!invalid, "coherent native configuration publication");
    std::printf("native display: %d failures\n", failures);
    return failures ? 1 : 0;
}
