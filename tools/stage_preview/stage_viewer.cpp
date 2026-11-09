// stage_viewer - fly through a stage's land drawn by the new renderer, live, straight from the disc
// (docs/lift-20261006/R_DESIGN.md: the importers + render::Renderer presenting on its own swap chain).
//
//   stage_viewer <files directory> <stage, e.g. stg0100> [--warp] [--no-vsync]
//
// Controls: W A S D move, Q / E down / up, hold the right mouse button to look, Shift faster, F far scenery on / off,
// C back-face culling note in the title, Esc quits. The window title shows the frame rate and the draw count.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "render/frame.h"
#include "render/renderer.h"
#include "stage_scene.h"

using namespace shadow;

namespace {

struct Input {
    bool keys[256] = {};
    bool looking = false;
    POINT last{};
    float yaw = 0, pitch = 0;
    bool resized = false;
    std::uint32_t width = 1600, height = 900;
    bool quit = false;
    bool toggle_far = false;
};

Input g_input;

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_DESTROY:
        g_input.quit = true;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE)
            g_input.quit = true;
        if (w == 'F' && (l & (1 << 30)) == 0)
            g_input.toggle_far = true;
        if (w < 256)
            g_input.keys[w] = true;
        return 0;
    case WM_KEYUP:
        if (w < 256)
            g_input.keys[w] = false;
        return 0;
    case WM_RBUTTONDOWN:
        g_input.looking = true;
        GetCursorPos(&g_input.last);
        SetCapture(window);
        return 0;
    case WM_RBUTTONUP:
        g_input.looking = false;
        ReleaseCapture();
        return 0;
    case WM_SIZE:
        if (LOWORD(l) > 0 && HIWORD(l) > 0) {
            g_input.width = LOWORD(l);
            g_input.height = HIWORD(l);
            g_input.resized = true;
        }
        return 0;
    default:
        return DefWindowProcW(window, message, w, l);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: stage_viewer <files dir> <stage> [--warp] [--no-vsync]\n");
        return 2;
    }
    const std::filesystem::path files = std::filesystem::u8path(argv[1]);
    const std::string stage_name = argv[2];
    bool warp = false, vsync = true;
    std::uint64_t frame_limit = 0;  // --frames N: quit after N frames (smoke runs)
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--warp") warp = true;
        else if (a == "--no-vsync") vsync = false;
        else if (a == "--frames" && i + 1 < argc) frame_limit = std::stoull(argv[++i]);
    }

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // the arrow
    window_class.lpszClassName = L"shadow.stage_viewer";
    RegisterClassW(&window_class);
    RECT rect{0, 0, static_cast<LONG>(g_input.width), static_cast<LONG>(g_input.height)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(0, window_class.lpszClassName, L"stage viewer", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                  CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                  window_class.hInstance, nullptr);
    if (window == nullptr)
        return 1;

    render::Renderer renderer;
    render::RendererOptions options;
    options.device.adapter = warp ? render::DeviceOptions::Adapter::Warp : render::DeviceOptions::Adapter::HighPerformance;
    std::string error;
    if (!renderer.create(options, &error) || !renderer.attach_window(window, g_input.width, g_input.height, &error)) {
        std::fprintf(stderr, "renderer: %s\n", error.c_str());
        return 1;
    }
    stage::Scene scene;
    if (!stage::load(renderer, files, stage_name, stage::LoadOptions{}, &scene, &error)) {
        std::fprintf(stderr, "stage: %s\n", error.c_str());
        return 1;
    }
    std::printf("%s: %zu land worlds, %zu textures, %zu draws on %s\n", stage_name.c_str(), scene.worlds,
                scene.textures, scene.draws.size(), renderer.device().caps().adapter_name.c_str());
    ShowWindow(window, SW_SHOW);

    // Start above one corner of the land, looking at its middle.
    const float* lo = scene.lo;
    const float* hi = scene.hi;
    const float extent = std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
                                   (hi[2] - lo[2]) * (hi[2] - lo[2]));
    render::Float3 position{(lo[0] + hi[0]) * 0.5f + (hi[0] - lo[0]) * 0.3f, hi[1] + (hi[1] - lo[1]) * 0.05f,
                            (lo[2] + hi[2]) * 0.5f + (hi[2] - lo[2]) * 0.3f};
    {
        const float dx = (lo[0] + hi[0]) * 0.5f - position.x;
        const float dz = (lo[2] + hi[2]) * 0.5f - position.z;
        g_input.yaw = std::atan2(dx, dz);
        g_input.pitch = -0.35f;
    }
    bool far_scenery = true;
    render::FrameBuilder builder;
    auto last = std::chrono::steady_clock::now();
    double title_timer = 0;
    int frames = 0;
    std::uint64_t tick = 0;
    const auto started = std::chrono::steady_clock::now();
    while (!g_input.quit && (frame_limit == 0 || tick < frame_limit)) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_input.quit)
            break;
        if (g_input.resized) {
            g_input.resized = false;
            if (!renderer.resize_window(g_input.width, g_input.height, &error))
                std::fprintf(stderr, "resize: %s\n", error.c_str());
        }
        if (g_input.toggle_far) {
            g_input.toggle_far = false;
            far_scenery = !far_scenery;
        }
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::min(0.1f, std::chrono::duration<float>(now - last).count());
        last = now;
        if (g_input.looking) {
            POINT p;
            GetCursorPos(&p);
            g_input.yaw += (p.x - g_input.last.x) * 0.004f;
            g_input.pitch = std::clamp(g_input.pitch - (p.y - g_input.last.y) * 0.004f, -1.5f, 1.5f);
            g_input.last = p;
        }
        const render::Float3 forward{std::sin(g_input.yaw) * std::cos(g_input.pitch), std::sin(g_input.pitch),
                                     std::cos(g_input.yaw) * std::cos(g_input.pitch)};
        // Right-handed world, y up: screen right is forward x up.
        const render::Float3 right{-forward.z, 0.0f, forward.x};
        const float rlen = std::sqrt(right.x * right.x + right.z * right.z);
        const float speed = extent * (g_input.keys[VK_SHIFT] ? 0.25f : 0.05f) * dt;
        auto move = [&](const render::Float3& dir, float amount) {
            position.x += dir.x * amount;
            position.y += dir.y * amount;
            position.z += dir.z * amount;
        };
        if (g_input.keys['W']) move(forward, speed);
        if (g_input.keys['S']) move(forward, -speed);
        if (g_input.keys['D']) move(render::Float3{right.x / rlen, 0, right.z / rlen}, -speed);
        if (g_input.keys['A']) move(render::Float3{right.x / rlen, 0, right.z / rlen}, speed);
        if (g_input.keys['E']) move(render::Float3{0, 1, 0}, speed);
        if (g_input.keys['Q']) move(render::Float3{0, 1, 0}, -speed);

        render::Camera camera;
        camera.frame = render::look_at(position,
                                       render::Float3{position.x + forward.x, position.y + forward.y,
                                                      position.z + forward.z},
                                       render::Float3{0, 1, 0});
        camera.view_window_y = std::tan(0.5f);
        camera.near_plane = extent * 0.0005f;
        camera.far_plane = extent * 6.0f;
        builder.begin_frame(++tick, render::Float4{0.55f, 0.65f, 0.78f, 1.0f});
        const render::ViewId view = builder.view(camera, render::ViewRect{});
        render::Clear clear;
        clear.colour = true;
        clear.depth = true;
        builder.begin_pass(view, render::PassKind::Scene, clear);
        stage::submit(scene, builder, far_scenery);
        builder.end_pass();
        if (!renderer.render(builder.finish(), vsync ? render::PresentMode::VSync : render::PresentMode::Immediate,
                             &error)) {
            std::fprintf(stderr, "render: %s\n", error.c_str());
            break;
        }
        ++frames;
        title_timer += dt;
        if (title_timer >= 0.5) {
            wchar_t title[256];
            std::swprintf(title, 256, L"stage viewer - %hs - %.0f fps - %llu draws - far scenery %ls (F)",
                          stage_name.c_str(), frames / title_timer,
                          static_cast<unsigned long long>(renderer.stats().draws), far_scenery ? L"on" : L"off");
            SetWindowTextW(window, title);
            frames = 0;
            title_timer = 0;
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("%llu frames in %.2f s (%.1f fps), last frame %llu draws\n", static_cast<unsigned long long>(tick),
                seconds, tick / std::max(seconds, 1e-9), static_cast<unsigned long long>(renderer.stats().draws));
    renderer.destroy();
    DestroyWindow(window);
    return 0;
}
