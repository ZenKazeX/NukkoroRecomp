// The Vulkan renderer's windowed check (stage V1): a Win32 window presented through the swapchain for a number of
// frames (the background cycling, split screen's three views), resized once on the way, then closed.
//
//   shadow_render_vk_window [--frames N] [--validation]
//
// Exit code 0: every frame rendered and presented and the validation layer (when on) reported nothing.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "render/frame.h"
#include "render_vk/renderer.h"

using namespace shadow;

namespace {

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}

}  // namespace

int main(int argc, char** argv) {
    int frames = 120;
    render_vk::RendererOptions options;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            frames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--validation") == 0)
            options.device.validation = true;
    }
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = L"ShadowRenderVkWindow";
    RegisterClassW(&wc);
    RECT rect{0, 0, 640, 360};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"render_vk window check", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                  CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                  instance, nullptr);
    if (window == nullptr)
        return 2;
    ShowWindow(window, SW_SHOWNOACTIVATE);

    render_vk::Renderer renderer;
    std::string error;
    if (!renderer.create(options, &error) ||
        !renderer.attach_window(render_vk::NativeWindow{window, instance}, 640, 360, &error)) {
        std::fprintf(stderr, "setup: %s\n", error.c_str());
        return 3;
    }
    render::FrameBuilder builder;
    render::Camera camera;
    int rendered = 0;
    for (int f = 0; f < frames; ++f) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                f = frames;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (f >= frames)
            break;
        if (f == frames / 2) {
            // Halfway: a resize, as a window drag gives one.
            SetWindowPos(window, nullptr, 0, 0, 800, 450, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT client;
            GetClientRect(window, &client);
            if (!renderer.resize_window(static_cast<std::uint32_t>(client.right),
                                        static_cast<std::uint32_t>(client.bottom), &error)) {
                std::fprintf(stderr, "resize: %s\n", error.c_str());
                return 4;
            }
        }
        const float t = static_cast<float>(f) / 60.0f;
        builder.begin_frame(static_cast<std::uint64_t>(f + 1),
                            render::Float4{0.5f + 0.5f * std::sin(t), 0.5f + 0.5f * std::sin(t + 2.1f),
                                           0.5f + 0.5f * std::sin(t + 4.2f), 1.0f});
        const render::ViewId left = builder.view(camera, {0.0f, 0.0f, 0.5f, 1.0f});
        const render::ViewId right = builder.view(camera, {0.5f, 0.0f, 0.5f, 1.0f});
        builder.view(camera, {0.0f, 0.0f, 1.0f, 1.0f}, render::Canvas{}, true);
        builder.begin_pass(left, render::PassKind::Scene, render::Clear{});
        builder.end_pass();
        builder.begin_pass(right, render::PassKind::Scene, render::Clear{});
        builder.end_pass();
        render::FramePacket packet = builder.finish();
        packet.content_aspect = 4.0f / 3.0f;  // pillar-boxed, as an in-engine cutscene is
        if (!renderer.render(packet, render_vk::PresentMode::VSync, &error)) {
            std::fprintf(stderr, "render frame %d: %s\n", f, error.c_str());
            return 5;
        }
        ++rendered;
    }
    const auto messages = renderer.take_debug_errors();
    for (const std::string& m : messages)
        std::printf("validation %s\n", m.c_str());
    const render_vk::Renderer::Stats stats = renderer.stats();
    std::printf("presented %d frames (renderer frames %llu, views over views %llu)\n", rendered,
                static_cast<unsigned long long>(stats.frames), static_cast<unsigned long long>(stats.views_over_views));
    renderer.destroy();
    DestroyWindow(window);
    const bool ok = rendered == frames && messages.empty();
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
