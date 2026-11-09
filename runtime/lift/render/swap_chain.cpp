#include "render/swap_chain.h"

#include <algorithm>
#include <cstdio>

#include "render/present_trace.h"

namespace shadow::render {

namespace {

bool fail(std::string* error, const std::string& what, HRESULT hr) {
    if (error != nullptr)
        *error = what + ": " + hresult_text(hr);
    return false;
}

}  // namespace

SwapChain::~SwapChain() {
    destroy();
}

bool SwapChain::create(Device& device, HWND window, std::uint32_t width, std::uint32_t height,
                       const SwapChainOptions& options, std::string* error) {
    destroy();
    device_ = &device;
    options_ = options;
    options_.buffers = std::clamp<std::uint32_t>(options.buffers, 2, kMaxBuffers);
    tearing_ = options.allow_tearing && device.caps().tearing;
    width_ = std::max<std::uint32_t>(width, 1);
    height_ = std::max<std::uint32_t>(height, 1);

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = options_.format;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = options_.buffers;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                 (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
    Com<IDXGISwapChain1> swap_chain;
    HRESULT hr = device.factory()->CreateSwapChainForHwnd(device.direct().get(), window, &desc, nullptr, nullptr,
                                                          &swap_chain);
    if (FAILED(hr))
        return fail(error, "CreateSwapChainForHwnd", hr);
    // The game decides fullscreen (a borderless window); DXGI must not switch modes on Alt+Enter.
    device.factory()->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    hr = swap_chain.As(&swap_chain_);
    if (FAILED(hr))
        return fail(error, "IDXGISwapChain3", hr);
    swap_chain_->SetMaximumFrameLatency(std::max<std::uint32_t>(options_.max_latency, 1));
    waitable_ = swap_chain_->GetFrameLatencyWaitableObject();
    if (!rtv_pool_.create(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kMaxBuffers, error))
        return false;
    return acquire_buffers(error);
}

void SwapChain::destroy() {
    if (device_ != nullptr && swap_chain_)
        device_->wait_idle();
    release_buffers();
    if (waitable_ != nullptr)
        CloseHandle(waitable_);
    waitable_ = nullptr;
    swap_chain_.Reset();
    device_ = nullptr;
}

bool SwapChain::acquire_buffers(std::string* error) {
    for (std::uint32_t i = 0; i < options_.buffers; ++i) {
        const HRESULT hr = swap_chain_->GetBuffer(i, IID_PPV_ARGS(&buffers_[i]));
        if (FAILED(hr))
            return fail(error, "GetBuffer", hr);
        buffers_[i]->SetName(L"shadow.back_buffer");
        rtvs_[i] = rtv_pool_.allocate();
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = options_.format;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device_->d3d()->CreateRenderTargetView(buffers_[i].Get(), &view, rtvs_[i]);
    }
    return true;
}

void SwapChain::release_buffers() {
    for (std::uint32_t i = 0; i < kMaxBuffers; ++i) {
        buffers_[i].Reset();
        rtv_pool_.free(rtvs_[i]);
        rtvs_[i] = {};
    }
}

bool SwapChain::wait_for_frame(std::uint32_t timeout_ms) {
    if (waitable_ == nullptr)
        return true;
    present_trace::record(present_trace::Ev::WaitBegin);
    // An APC delivered to this thread ends an alertable wait with WAIT_IO_COMPLETION and no credit; it used to count as
    // one (the result was ignored, Codex review 2026-11-05). Wait again for the time left.
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    DWORD result = WaitForSingleObjectEx(waitable_, timeout_ms, TRUE);
    while (result == WAIT_IO_COMPLETION) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            result = WAIT_TIMEOUT;
            break;
        }
        result = WaitForSingleObjectEx(waitable_, static_cast<DWORD>(deadline - now), TRUE);
    }
    const bool signalled = result == WAIT_OBJECT_0;
    present_trace::record(present_trace::Ev::WaitEnd, signalled ? 1u : 0u);
    return signalled;
}

bool SwapChain::try_take_credit() {
    return waitable_ != nullptr && WaitForSingleObjectEx(waitable_, 0, FALSE) == WAIT_OBJECT_0;
}

bool SwapChain::last_vblank(std::uint64_t& refresh_count, std::int64_t& vblank_ns) const {
    if (!swap_chain_)
        return false;
    DXGI_FRAME_STATISTICS stats{};
    if (FAILED(swap_chain_->GetFrameStatistics(&stats)) || stats.SyncRefreshCount == 0 || stats.SyncQPCTime.QuadPart <= 0)
        return false;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const std::uint64_t f = static_cast<std::uint64_t>(frequency.QuadPart);
    const std::uint64_t q = static_cast<std::uint64_t>(stats.SyncQPCTime.QuadPart);
    refresh_count = stats.SyncRefreshCount;
    vblank_ns = static_cast<std::int64_t>((q / f) * 1000000000ull + (q % f) * 1000000000ull / f);  // steady_clock's scale
    return true;
}

std::uint32_t SwapChain::current() const {
    return swap_chain_ ? swap_chain_->GetCurrentBackBufferIndex() : 0;
}

bool SwapChain::present(PresentMode mode, std::string* error) {
    const UINT interval = mode == PresentMode::VSync ? 1 : 0;
    const UINT flags = (mode == PresentMode::Immediate && tearing_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    present_trace::record(present_trace::Ev::PresentBegin, interval, flags);
    const HRESULT hr = swap_chain_->Present(interval, flags);
    if (present_trace::enabled()) {
        // Measurement only (GEKKO_PRESENT_TRACE): which present the display took at which vblank. These two calls are
        // made only while the trace is on.
        UINT last = 0;
        swap_chain_->GetLastPresentCount(&last);
        present_trace::record(present_trace::Ev::PresentEnd, static_cast<std::uint32_t>(hr), last);
        DXGI_FRAME_STATISTICS stats{};
        const HRESULT shr = swap_chain_->GetFrameStatistics(&stats);
        if (SUCCEEDED(shr)) {
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            const std::uint64_t f = static_cast<std::uint64_t>(frequency.QuadPart);
            const std::uint64_t q = static_cast<std::uint64_t>(stats.SyncQPCTime.QuadPart);
            const std::uint64_t ns = (q / f) * 1000000000ull + (q % f) * 1000000000ull / f;  // steady_clock's scale
            present_trace::record(present_trace::Ev::Stats, stats.PresentCount, stats.PresentRefreshCount,
                                  stats.SyncRefreshCount, ns);
        } else {
            present_trace::record(present_trace::Ev::Stats, ~std::uint64_t{0}, static_cast<std::uint32_t>(shr));
        }
        // How DXGI says the last displayed frame reached the screen: composed by the desktop compositor, as a
        // hardware overlay / independent flip, or neither (DXGI_FRAME_PRESENTATION_MODE).
        IDXGISwapChainMedia* media = nullptr;
        HRESULT mhr = swap_chain_->QueryInterface(IID_PPV_ARGS(&media));
        if (SUCCEEDED(mhr)) {
            DXGI_FRAME_STATISTICS_MEDIA media_stats{};
            mhr = media->GetFrameStatisticsMedia(&media_stats);
            media->Release();
            if (SUCCEEDED(mhr))
                present_trace::record(present_trace::Ev::PresentMode, static_cast<std::uint32_t>(media_stats.CompositionMode),
                                      media_stats.ApprovedPresentDuration, media_stats.PresentCount);
        }
        if (FAILED(mhr))
            present_trace::record(present_trace::Ev::PresentMode, ~std::uint64_t{0}, static_cast<std::uint32_t>(mhr));
    }
    if (hr == DXGI_STATUS_OCCLUDED)
        return true;
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            // A lost device (TDR, driver reset or update) is not recreated: every later frame fails the same way.
            // Name the cause once, where the user sees it, instead of a silently frozen window.
            static bool reported = false;
            const HRESULT reason = device_->d3d()->GetDeviceRemovedReason();
            if (!reported) {
                reported = true;
                std::fprintf(stderr, "[render] GPU device lost (Present 0x%08X, reason 0x%08X); frames stop here\n",
                             static_cast<unsigned>(hr), static_cast<unsigned>(reason));
            }
            return fail(error, "Present: device lost", reason);
        }
        return fail(error, "Present", hr);
    }
    return true;
}

bool SwapChain::resize(std::uint32_t width, std::uint32_t height, std::string* error) {
    width = std::max<std::uint32_t>(width, 1);
    height = std::max<std::uint32_t>(height, 1);
    if (width == width_ && height == height_)
        return true;
    device_->wait_idle();
    release_buffers();
    const HRESULT hr = swap_chain_->ResizeBuffers(
        0, width, height, DXGI_FORMAT_UNKNOWN,
        DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u));
    if (FAILED(hr)) {
        // The buffers are released: forget the size, so the next resize to any size (this one again) retries.
        width_ = height_ = 0;
        return fail(error, "ResizeBuffers", hr);
    }
    width_ = width;
    height_ = height;
    return acquire_buffers(error);
}

}  // namespace shadow::render
