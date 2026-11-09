#include "render/gpu_device.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include <D3D12MemAlloc.h>

namespace shadow::render {

namespace {

std::string narrow(const wchar_t* text) {
    std::string out;
    if (text == nullptr)
        return out;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1)
        return out;
    out.resize(static_cast<std::size_t>(bytes - 1));
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

bool fail(std::string* error, const std::string& what, HRESULT hr) {
    if (error != nullptr)
        *error = what + ": " + hresult_text(hr);
    return false;
}

bool fail(std::string* error, const std::string& what) {
    if (error != nullptr)
        *error = what;
    return false;
}

// GPU clock boost (NVIDIA only; every other vendor: nothing happens). The driver's power policy keeps this light load
// at its lowest clocks and changes power state mid-play; measured (docs/PERF_60FPS_2026-11-04.md "Dropped refreshes"):
// at 2560x1440 the GPU runs ~2.4x slower for ~18 frames around a P5 -> P8 change and misses 5 refreshes, at 5K the
// stage-start smoke burst overruns 16.7 ms at 885 MHz. NvAPI_D3D_SetSleepMode's bLowLatencyBoost asks the driver to
// keep maximum clocks for this device ("even in scenarios where it is idle most of the frame"). Only the boost is
// requested: low latency mode and the frame limit stay off, so pacing is unchanged. It changes when the GPU finishes,
// never what it computes. The driver's own nvapi64.dll is loaded at run time through nvapi_QueryInterface; the ids
// and the parameter layout are NVIDIA's (NVAPI SDK, MIT: nvapi_interface.h, nvapi.h NV_SET_SLEEP_MODE_PARAMS_V1).
// The setting belongs to the device and ends with it; nothing is written to the driver's profiles.
// SHADOW_R2_GPU_BOOST=0 turns it off.
void request_gpu_clock_boost(ID3D12Device* device) {
    if (const char* env = std::getenv("SHADOW_R2_GPU_BOOST"); env != nullptr && env[0] == '0')
        return;
    HMODULE nvapi = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (nvapi == nullptr)
        return;  // not an NVIDIA driver
    using QueryInterface = void* (*)(unsigned int);
    const auto query = reinterpret_cast<QueryInterface>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (query == nullptr)
        return;
    using Initialize = int (*)();
    using SetSleepMode = int (*)(IUnknown*, void*);
    const auto initialize = reinterpret_cast<Initialize>(query(0x0150E828u));        // NvAPI_Initialize
    const auto set_sleep_mode = reinterpret_cast<SetSleepMode>(query(0xAC1CA9E0u));  // NvAPI_D3D_SetSleepMode
    if (initialize == nullptr || set_sleep_mode == nullptr || initialize() != 0) {
        std::fprintf(stderr, "[gpu] clock boost: unavailable\n");
        return;
    }
    struct SleepModeParamsV1 {  // NV_SET_SLEEP_MODE_PARAMS_V1
        std::uint32_t version;
        std::uint8_t low_latency_mode;
        std::uint8_t low_latency_boost;
        std::uint32_t minimum_interval_us;
        std::uint8_t use_markers_to_optimize;
        std::uint8_t reserved[31];
    };
    static_assert(sizeof(SleepModeParamsV1) == 44);
    SleepModeParamsV1 params{};
    params.version = static_cast<std::uint32_t>(sizeof(SleepModeParamsV1)) | (1u << 16);  // MAKE_NVAPI_VERSION(.., 1)
    params.low_latency_boost = 1;
    const int status = set_sleep_mode(device, &params);
    std::fprintf(stderr, "[gpu] clock boost: %s (%d)\n", status == 0 ? "on" : "refused", status);
    // nvapi64.dll stays loaded: the driver holds the setting for the device's lifetime.
}

D3D_SHADER_MODEL highest_shader_model(ID3D12Device* device) {
    D3D12_FEATURE_DATA_SHADER_MODEL data{D3D_SHADER_MODEL_6_8};
    while (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &data, sizeof(data)))) {
        if (data.HighestShaderModel <= D3D_SHADER_MODEL_5_1)
            return D3D_SHADER_MODEL_5_1;
        data.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(data.HighestShaderModel - 1);
    }
    return data.HighestShaderModel;
}

DeviceCaps query_caps(IDXGIFactory6* factory, IDXGIAdapter1* adapter, ID3D12Device* device) {
    DeviceCaps caps;
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    caps.adapter_name = narrow(desc.Description);
    caps.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    caps.vendor_id = desc.VendorId;
    caps.device_id = desc.DeviceId;
    caps.dedicated_video_memory = desc.DedicatedVideoMemory;
    caps.shader_model = highest_shader_model(device);
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
        caps.binding_tier = options.ResourceBindingTier;
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12))))
        caps.enhanced_barriers = options12.EnhancedBarriersSupported != FALSE;
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options16, sizeof(options16))))
        caps.gpu_upload_heap = options16.GPUUploadHeapSupported != FALSE;
    BOOL tearing = FALSE;
    if (SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
        caps.tearing = tearing != FALSE;
    return caps;
}

// The renderer's floor: shaders index ResourceDescriptorHeap / SamplerDescriptorHeap directly.
bool meets_floor(const DeviceCaps& caps) {
    return caps.shader_model >= D3D_SHADER_MODEL_6_6 && caps.binding_tier >= D3D12_RESOURCE_BINDING_TIER_3;
}

}  // namespace

std::string hresult_text(HRESULT hr) {
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08X", static_cast<unsigned>(hr));
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::string text = code;
    if (length != 0 && message != nullptr) {
        std::string described = narrow(message);
        while (!described.empty() && (described.back() == '\n' || described.back() == '\r' || described.back() == ' '))
            described.pop_back();
        text += " (" + described + ")";
    }
    if (message != nullptr)
        LocalFree(message);
    return text;
}

// ---- Queue --------------------------------------------------------------------------------------------------

Queue::~Queue() {
    reset();
}

void Queue::reset() {
    if (event_ != nullptr)
        CloseHandle(event_);
    event_ = nullptr;
    fence_.Reset();
    queue_.Reset();
    last_value_ = 0;
}

bool Queue::create(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type, const wchar_t* name, std::string* error) {
    reset();
    type_ = type;
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = type;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    HRESULT hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue_));
    if (FAILED(hr))
        return fail(error, "CreateCommandQueue", hr);
    queue_->SetName(name);
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(hr))
        return fail(error, "CreateFence", hr);
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_ == nullptr)
        return fail(error, "CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
    last_value_ = 0;
    return true;
}

std::uint64_t Queue::submit(ID3D12CommandList* const* lists, std::uint32_t count) {
    if (count != 0)
        queue_->ExecuteCommandLists(count, lists);
    return signal();
}

std::uint64_t Queue::signal() {
    // One submitting thread per queue: the increment is atomic only so that a mark taken on another thread reads a
    // whole value.
    const std::uint64_t value = last_value_.fetch_add(1, std::memory_order_acq_rel) + 1;
    queue_->Signal(fence_.Get(), value);
    return value;
}

void Queue::wait_for(const Queue& other, std::uint64_t value) {
    if (value != 0)
        queue_->Wait(other.fence(), value);
}

std::uint64_t Queue::completed() const {
    return fence_ ? fence_->GetCompletedValue() : 0;
}

void Queue::wait(std::uint64_t value) {
    if (value == 0 || completed() >= value)
        return;
    if (SUCCEEDED(fence_->SetEventOnCompletion(value, event_)))
        WaitForSingleObject(event_, INFINITE);
}

// ---- Device -------------------------------------------------------------------------------------------------

Device::~Device() {
    destroy();
}

bool Device::create(const DeviceOptions& options, std::string* error) {
    destroy();
    if (options.debug_layer) {
        Com<ID3D12Debug1> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            if (options.gpu_validation)
                debug->SetEnableGPUBasedValidation(TRUE);
        }
    }
    HRESULT hr = CreateDXGIFactory2(options.debug_layer ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory_));
    if (FAILED(hr) && options.debug_layer)
        hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
    if (FAILED(hr))
        return fail(error, "CreateDXGIFactory2", hr);

    std::string rejected;
    if (options.adapter == DeviceOptions::Adapter::Warp) {
        hr = factory_->EnumWarpAdapter(IID_PPV_ARGS(&adapter_));
        if (FAILED(hr))
            return fail(error, "EnumWarpAdapter", hr);
        hr = D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_));
        if (FAILED(hr))
            return fail(error, "D3D12CreateDevice (WARP)", hr);
        caps_ = query_caps(factory_.Get(), adapter_.Get(), device_.Get());
        if (!meets_floor(caps_))
            return fail(error, "WARP lacks Shader Model 6.6 / resource binding tier 3");
    } else {
        Com<IDXGIAdapter1> candidate;
        const DXGI_GPU_PREFERENCE preference = options.adapter == DeviceOptions::Adapter::LowPower
                                                   ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
                                                   : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
        for (UINT i = 0;
             factory_->EnumAdapterByGpuPreference(i, preference, IID_PPV_ARGS(&candidate)) != DXGI_ERROR_NOT_FOUND;
             ++i, candidate.Reset()) {
            DXGI_ADAPTER_DESC1 desc{};
            candidate->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
                continue;
            Com<ID3D12Device8> device;
            if (FAILED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
                rejected += " " + narrow(desc.Description) + " (no feature level 12_0);";
                continue;
            }
            const DeviceCaps caps = query_caps(factory_.Get(), candidate.Get(), device.Get());
            if (!meets_floor(caps)) {
                rejected += " " + caps.adapter_name + " (below Shader Model 6.6 / binding tier 3);";
                continue;
            }
            adapter_ = candidate;
            device_ = device;
            caps_ = caps;
            break;
        }
        if (!device_)
            return fail(error, "no GPU with Shader Model 6.6 and resource binding tier 3:" + rejected);
    }

    if (options.debug_layer) {
        Com<ID3D12InfoQueue> info;
        if (SUCCEEDED(device_.As(&info))) {
            info->SetMuteDebugOutput(FALSE);
            info->SetMessageCountLimit(4096);
            // Pooled render targets carry one fixed optimized clear value per kind (frame_graph.cpp TexturePool,
            // G-R1); a clear to another value is correct and only loses the fast-clear path. Its performance warning
            // would fill the stored-message limit every few frames and push real errors out, so it is not stored.
            D3D12_MESSAGE_ID deny[] = {D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                                       D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE};
            D3D12_INFO_QUEUE_FILTER filter{};
            filter.DenyList.NumIDs = static_cast<UINT>(sizeof(deny) / sizeof(deny[0]));
            filter.DenyList.pIDList = deny;
            info->AddStorageFilterEntries(&filter);
        }
    }

#if !defined(GEKKO_SHIPPING)
    // Measurement only (docs/GPU_FILL.md): SHADOW_R2_STABLE_POWER=1 asks the driver for its stable profiling clocks
    // for this process's lifetime (Windows developer mode), so GPU-time A/B runs are not decided by the driver's
    // clock state. Never in a shipping build.
    if (const char* stable = std::getenv("SHADOW_R2_STABLE_POWER"); stable != nullptr && stable[0] == '1') {
        const HRESULT stable_hr = device_->SetStablePowerState(TRUE);
        std::fprintf(stderr, "[gpu] stable power state: %s (0x%08lX)\n", SUCCEEDED(stable_hr) ? "on" : "refused",
                     static_cast<unsigned long>(stable_hr));
    }
#endif
    request_gpu_clock_boost(device_.Get());
    if (!direct_.create(device_.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, L"shadow.direct", error))
        return false;
    if (!copy_.create(device_.Get(), D3D12_COMMAND_LIST_TYPE_COPY, L"shadow.copy", error))
        return false;

    D3D12MA::ALLOCATOR_DESC allocator_desc{};
    allocator_desc.pDevice = device_.Get();
    allocator_desc.pAdapter = adapter_.Get();
    // Every render target is cleared or fully written before it is read (the frame graph refuses a read of
    // unwritten contents), and every asset resource is filled by its upload, so heaps need not be zeroed.
    allocator_desc.Flags = static_cast<D3D12MA::ALLOCATOR_FLAGS>(D3D12MA::ALLOCATOR_FLAG_DEFAULT_POOLS_NOT_ZEROED |
                                                                 D3D12MA::ALLOCATOR_FLAG_MSAA_TEXTURES_ALWAYS_COMMITTED);
    hr = D3D12MA::CreateAllocator(&allocator_desc, &allocator_);
    if (FAILED(hr))
        return fail(error, "D3D12MA::CreateAllocator", hr);
    return true;
}

void Device::destroy() {
    if (device_) {
        wait_idle();
    }
    {
        std::lock_guard<std::mutex> lock(retired_mutex_);
        retired_.clear();
    }
    if (allocator_ != nullptr) {
        allocator_->Release();
        allocator_ = nullptr;
    }
    copy_.reset();
    direct_.reset();
    device_.Reset();
    adapter_.Reset();
    factory_.Reset();
    caps_ = DeviceCaps{};
}

void Device::retire(Com<IUnknown> object) {
    retire(std::move(object), mark());
}

void Device::retire(Com<IUnknown> object, const GpuMark& mark) {
    if (!object)
        return;
    if (reached(mark))
        return;  // the GPU is already past every use: release now
    std::lock_guard<std::mutex> lock(retired_mutex_);
    retired_.push_back(Retired{mark, std::move(object)});
}

void Device::collect() {
    // Marks are taken from fence values that only grow, so the queue is ordered and the scan stops at the first
    // object the GPU may still use. (Marks retired from different threads interleave; an object behind a younger
    // mark is released one collect later, never early.) The objects are released outside the lock.
    std::deque<Retired> done;
    {
        std::lock_guard<std::mutex> lock(retired_mutex_);
        while (!retired_.empty() && reached(retired_.front().mark)) {
            done.push_back(std::move(retired_.front()));
            retired_.pop_front();
        }
    }
}

void Device::wait_idle() {
    if (direct_.get() != nullptr)
        direct_.wait_idle();
    if (copy_.get() != nullptr)
        copy_.wait_idle();
    collect();
}

std::vector<std::string> Device::take_debug_errors() {
    std::vector<std::string> out;
    Com<ID3D12InfoQueue> info;
    if (!device_ || FAILED(device_.As(&info)))
        return out;
    const UINT64 count = info->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T length = 0;
        if (FAILED(info->GetMessage(i, nullptr, &length)) || length == 0)
            continue;
        std::vector<unsigned char> storage(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(info->GetMessage(i, message, &length)))
            continue;
        if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
            out.emplace_back(message->pDescription, message->DescriptionByteLength > 0 ? message->DescriptionByteLength - 1 : 0);
    }
    info->ClearStoredMessages();
    return out;
}

Device::Budget Device::local_budget() const {
    Budget out;
    if (allocator_ == nullptr)
        return out;
    D3D12MA::Budget local{};
    allocator_->GetBudget(&local, nullptr);
    out.budget = local.BudgetBytes;
    out.usage = local.UsageBytes;
    return out;
}

}  // namespace shadow::render
