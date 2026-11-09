// Real DXGI swapchains on hidden test-owned windows. No game state is loaded.
#include "gekko/display.h"
#include "gekko/plume_present.h"
#include "plume_d3d12.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>

using namespace gekko;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Window {
    HWND hwnd = nullptr;
    Window(int width, int height) {
        hwnd = CreateWindowExW(0, L"STATIC", L"Shadow swapchain test", WS_POPUP,
                               0, 0, width, height, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
        require(hwnd != nullptr, "create hidden test window");
    }
    ~Window() { if (hwnd) DestroyWindow(hwnd); }
    void size(int width, int height) {
        require(SetWindowPos(hwnd, nullptr, 0, 0, width, height,
                            SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE) != 0,
                "resize hidden test window");
        RECT rect{};
        require(GetClientRect(hwnd, &rect) && rect.right == width && rect.bottom == height,
                "test window must have requested client dimensions");
    }
};

constexpr u32 kColour = 0xff214263u;

void verify_dump(const std::filesystem::path& path, int width, int height) {
    std::ifstream file(path, std::ios::binary);
    std::string magic;
    int seen_width = 0, seen_height = 0, maximum = 0;
    file >> magic >> seen_width >> seen_height >> maximum;
    require(file.good() && magic == "P6" && seen_width == width &&
                seen_height == height && maximum == 255, "backbuffer capture dimensions");
    require(file.get() == '\n', "PPM header terminator");
    const auto content = fit_content_rect(width, height, 4.0f / 3.0f);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const bool inside = x >= content.x && x < content.x + content.width &&
                            y >= content.y && y < content.y + content.height;
        const u32 expected = inside ? kColour : 0;
        for (int shift : {16, 8, 0}) {
            require(file.get() == static_cast<int>((expected >> shift) & 255u),
                    "resized backbuffer must retain source colour and black bars");
        }
    }
    require(file.peek() == std::char_traits<char>::eof(), "capture byte count");
}

struct Pins {
    std::set<u64> outstanding;
    bool duplicate = false;
    u64 returned = 0;
    static void release(void* context, u32, u64 generation) {
        auto& pins = *static_cast<Pins*>(context);
        if (pins.outstanding.erase(generation) != 1u) pins.duplicate = true;
        ++pins.returned;
    }
};

void presenter_path(plume::RenderDevice& device, plume::RenderCommandQueue& queue,
                    plume::RenderTexture* texture, plume::RenderTexture* previous_texture, int mode,
                    const std::filesystem::path& output) {
    Window window(64, 48);
    Pins pins;
    PlumePresenter presenter;
    presenter.set_vsync(false);
    presenter.set_gpu_blit(mode != 0);
    presenter.set_verify(mode == 0);
    presenter.set_native_release_hook(Pins::release, &pins);
    require(presenter.create(window.hwnd, &device, &queue, nullptr), "create presenter");
    require(presenter.last_error().empty() && !presenter.fence_failed(),
            "new presenter has no terminal retirement failure");
    std::vector<u32> pixels(32u * 24u, kColour);
    u64 generation = 0;
    u64 issued_pins = 0;
    const auto draw = [&](int width, int height, bool capture) {
        if (mode == 0) pixels.assign(static_cast<std::size_t>(width) * height, kColour);
        const auto path = output / ("mode" + std::to_string(mode) + "-" +
                                     std::to_string(generation) + ".ppm");
        if (capture && mode != 0) presenter.set_present_dump(path.string().c_str(), 1);
        ++generation;
        if (mode == 2) {
            PlumePresenter::NativePin pin{0, generation * 2u, true, nullptr};
            PlumePresenter::NativePin previous_pin{0, generation * 2u + 1u, true, nullptr};
            pins.outstanding.insert(pin.generation);
            pins.outstanding.insert(previous_pin.generation);
            issued_pins += 2u;
            const bool drawn = presenter.present_native(texture, previous_texture, 0.5f, 32, 24,
                                                          4.0f / 3.0f, &pin, &previous_pin);
            require(drawn == (width > 0 && height > 0), "native present/skip result");
        } else {
            // A valid frame is supplied even while the window is 0x0.
            if (mode == 0 && width == 0) pixels.assign(32u * 24u, kColour);
            presenter.present(pixels.data(), mode == 0 && width > 0 ? width : 32,
                               mode == 0 && height > 0 ? height : 24,
                               mode == 0 && width > 0 ? float(width) / height : 4.0f / 3.0f);
        }
        if (!presenter.available() || presenter.failed()) {
            throw std::runtime_error("presenter failed: mode=" + std::to_string(mode) +
                                     " size=" + std::to_string(width) + "x" +
                                     std::to_string(height) + " generation=" +
                                     std::to_string(generation));
        }
        require(presenter.last_error().empty() && !presenter.fence_failed(),
                "resize/minimise/present must not latch a fence failure");
        if (capture && mode != 0) verify_dump(path, width, height);
    };

    for (const auto size : {std::pair{64,48}, {97,71}, {32,24}, {80,96}, {64,48}}) {
        window.size(size.first, size.second);
        draw(size.first, size.second, true);
        // Leave submissions in flight to exercise drain-before-recreation.
        for (int i = 0; i < 4; ++i) draw(size.first, size.second, false);
        const auto before = presenter.present_stats();
        window.size(0, 0);
        draw(0, 0, false);
        require(presenter.present_stats().presents == before.presents &&
                    presenter.present_stats().presents_skipped_minimised ==
                        before.presents_skipped_minimised + 1u,
                "zero-area window must skip without a DXGI Present");
    }
    window.size(64, 48);
    draw(64, 48, true);
    if (mode == 0) {
        const auto report = presenter.verify_report();
        require(report.frames == 26u && report.skipped == 0u &&
                    report.mismatched_pixels == 0u && report.mismatched_frames == 0u,
                "CPU upload must remain pixel exact across resize and restore");
    }
    presenter.destroy();
    require(presenter.last_error().empty() && !presenter.fence_failed(),
            "explicit destroy must retire successfully");
    require(pins.outstanding.empty() && !pins.duplicate &&
                pins.returned == issued_pins,
            "native pins must be released exactly once, including skipped presents");
    const auto returned_after_stop = pins.returned;
    presenter.destroy();
    require(pins.returned == returned_after_stop && presenter.last_error().empty(),
            "repeated explicit destroy must not release a lease twice");
    require(presenter.create(window.hwnd, &device, &queue, nullptr),
            "healthy presenter can recreate after explicit retirement");
    require(presenter.available() && !presenter.fence_failed() && presenter.last_error().empty(),
            "healthy recreate must preserve independent nonterminal status");
    for (int i = 0; i < 4; ++i) draw(64, 48, false);
    presenter.retire_completed_presents();
    presenter.destroy();
    require(presenter.last_error().empty() && !presenter.fence_failed() &&
                pins.outstanding.empty() && !pins.duplicate &&
                pins.returned == issued_pins,
            "recreated presenter must retire all source leases exactly once");
    std::cout << "PASS presenter mode " << mode << ": resize, zero-area, restore, pixels, pins\n";
}

void swapchain_contract(plume::RenderDevice& device, plume::RenderCommandQueue& queue) {
    Window first(64,48), second(80,60), empty(0,0);
    const auto make = [&](HWND window, u32 count = 3u) {
        return queue.createSwapChain(plume::RenderSwapChainDesc(
            window, plume::RenderFormat::B8G8R8A8_UNORM, count, false, 1));
    };
    auto a = make(first.hwnd);
    auto b = make(second.hwnd);
    require(a && b, "independent window swapchains on one device");
    auto* native = static_cast<plume::D3D12SwapChain*>(a.get());
    auto transition = queue.createCommandList();
    auto transition_fence = device.createCommandFence();
    require(transition && transition_fence, "create backbuffer transition resources");
    transition->begin();
    transition->barriers(plume::RenderBarrierStage::COPY,
        plume::RenderTextureBarrier(a->getTexture(0), plume::RenderTextureLayout::COPY_SOURCE));
    transition->end();
    queue.executeCommandLists(transition.get(), transition_fence.get());
    queue.waitForCommandFence(transition_fence.get());
    auto* held = static_cast<plume::D3D12Texture*>(a->getTexture(0))->d3d;
    held->AddRef();
    first.size(96,72);
    const bool failed_resize = !a->resize();
    held->Release();
    require(failed_resize && a->getWidth() == 64u && a->getHeight() == 48u &&
                !a->isEmpty() && a->getTexture(0) != nullptr,
            "failed ResizeBuffers must preserve the old usable buffer metadata");
    const auto* restored = static_cast<plume::D3D12Texture*>(a->getTexture(0));
    require(restored->resourceStates == D3D12_RESOURCE_STATE_COPY_SOURCE &&
                restored->layout == plume::RenderTextureLayout::COPY_SOURCE,
            "failed resize must preserve the existing backbuffer resource state");
    require(a->resize() && a->getWidth() == 96u && a->getHeight() == 72u,
            "resize must recover after the external backbuffer reference is released");
    const auto* replacement = static_cast<plume::D3D12Texture*>(a->getTexture(0));
    require(replacement->resourceStates == D3D12_RESOURCE_STATE_PRESENT &&
                replacement->layout == plume::RenderTextureLayout::PRESENT,
            "successful resize must initialize replacement backbuffers in PRESENT");
    require(b->getWidth() == 80u && b->getHeight() == 60u && !b->needsResize(),
            "resizing one window must not touch another swapchain");
    first.size(0,0);
    require(!a->resize() && a->getWidth() == 96u && !a->isEmpty(),
            "zero-size resize must not destroy committed backbuffers");
    first.size(96,72);
    require(!a->needsResize(), "same-size restore must reuse existing backbuffers");
    HANDLE wait_handle = native->waitableObject;
    require(wait_handle != nullptr, "swapchain owns its latency handle");
    a.reset();
    DWORD flags = 0;
    require(!GetHandleInformation(wait_handle, &flags) && GetLastError() == ERROR_INVALID_HANDLE,
            "destroy must close the swapchain latency handle");
    // Flip chains cannot have one buffer: exercise real DXGI creation failure.
    require(!make(first.hwnd, 1u), "failed creation must return null and clean up safely");
    require(make(first.hwnd) != nullptr, "failed creation must not poison the window");
    auto zero = make(empty.hwnd);
    require(zero && !zero->isEmpty(), "zero-area startup must provision a minimal valid surface");
    empty.size(40,30);
    require(zero->resize() && zero->getWidth() == 40u && zero->getHeight() == 30u,
            "zero-area startup must resize normally on restore");
    require(SUCCEEDED(static_cast<plume::D3D12Device&>(device).d3d->GetDeviceRemovedReason()),
            "device must remain healthy");
    std::cout << "PASS swapchain creation failure, resize failure/retry, multi-window, handle cleanup\n";
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: swapchain_lifecycle_tests <capture-directory>");
        std::filesystem::create_directories(argv[1]);
        plume::D3D12Interface render_interface;
        if (!render_interface.isValid()) return 77;
        auto device = render_interface.createDevice("");
        if (!device) return 77;
        auto queue = device->createCommandQueue(plume::RenderCommandListType::DIRECT);
        require(queue != nullptr, "create test queue");
        auto source = device->createTexture(plume::RenderTextureDesc::Texture2D(
            32, 24, 1, plume::RenderFormat::B8G8R8A8_UNORM));
        auto previous_source = device->createTexture(plume::RenderTextureDesc::Texture2D(
            32, 24, 1, plume::RenderFormat::B8G8R8A8_UNORM));
        auto upload = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(64u * 24u * 4u));
        auto command = queue->createCommandList();
        auto fence = device->createCommandFence();
        require(source && previous_source && upload && command && fence, "create source upload resources");
        auto* mapped = static_cast<u32*>(upload->map());
        require(mapped != nullptr, "map source upload");
        std::fill(mapped, mapped + 64u * 24u, kColour);
        upload->unmap();
        command->begin();
        for (auto* texture : {source.get(), previous_source.get()}) {
            command->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::COPY_DEST));
            command->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(texture),
                plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(),
                    plume::RenderFormat::B8G8R8A8_UNORM, 32, 24, 1, 64));
            command->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::SHADER_READ));
        }
        command->end();
        queue->executeCommandLists(command.get(), fence.get());
        queue->waitForCommandFence(fence.get());
        for (int mode = 0; mode < 3; ++mode)
            presenter_path(*device, *queue, source.get(), previous_source.get(), mode, argv[1]);
        swapchain_contract(*device, *queue);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL swapchain_lifecycle_tests: " << error.what() << '\n';
        return 1;
    }
}
