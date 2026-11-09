#include "render/descriptors.h"

namespace shadow::render {

// ---- BindlessHeap -------------------------------------------------------------------------------------------

bool BindlessHeap::create(Device& device, std::uint32_t persistent_capacity, std::uint32_t transient_per_frame,
                          std::uint32_t frames_in_flight, std::string* error) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = persistent_capacity + transient_per_frame * frames_in_flight;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    const HRESULT hr = device.d3d()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_));
    if (FAILED(hr)) {
        if (error != nullptr)
            *error = "CreateDescriptorHeap (bindless): " + hresult_text(hr);
        return false;
    }
    heap_->SetName(L"shadow.bindless");
    increment_ = device.d3d()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    std::lock_guard<std::mutex> lock(persistent_mutex_);
    persistent_capacity_ = persistent_capacity;
    persistent_next_ = 0;
    persistent_in_use_ = 0;
    free_list_.clear();
    pending_.clear();
    transient_per_frame_ = transient_per_frame;
    transient_base_ = persistent_capacity;
    transient_used_ = 0;
    exhausted_ = 0;
    return true;
}

std::uint32_t BindlessHeap::allocate() {
    std::lock_guard<std::mutex> lock(persistent_mutex_);
    std::uint32_t index = kNoDescriptor;
    if (!free_list_.empty()) {
        index = free_list_.back();
        free_list_.pop_back();
    } else if (persistent_next_ < persistent_capacity_) {
        index = persistent_next_++;
    } else {
        ++exhausted_;
        return kNoDescriptor;
    }
    ++persistent_in_use_;
    return index;
}

void BindlessHeap::free(std::uint32_t index, const GpuMark& mark) {
    if (index == kNoDescriptor || index >= persistent_capacity_)
        return;
    std::lock_guard<std::mutex> lock(persistent_mutex_);
    pending_.push_back(Freed{mark, index});
    --persistent_in_use_;
}

void BindlessHeap::reclaim(const Device& device) {
    std::lock_guard<std::mutex> lock(persistent_mutex_);
    while (!pending_.empty() && device.reached(pending_.front().mark)) {
        free_list_.push_back(pending_.front().index);
        pending_.pop_front();
    }
}

void BindlessHeap::begin_frame(std::uint32_t frame_slot) {
    transient_base_ = persistent_capacity_ + frame_slot * transient_per_frame_;
    transient_used_ = 0;
}

std::uint32_t BindlessHeap::allocate_transient() {
    if (transient_used_ >= transient_per_frame_) {
        ++exhausted_;
        return kNoDescriptor;
    }
    return transient_base_ + transient_used_++;
}

D3D12_CPU_DESCRIPTOR_HANDLE BindlessHeap::cpu(std::uint32_t index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * increment_;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE BindlessHeap::gpu(std::uint32_t index) const {
    D3D12_GPU_DESCRIPTOR_HANDLE handle = heap_->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * increment_;
    return handle;
}

// ---- CpuDescriptorPool --------------------------------------------------------------------------------------

bool CpuDescriptorPool::create(Device& device, D3D12_DESCRIPTOR_HEAP_TYPE type, std::uint32_t capacity,
                               std::string* error) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = capacity;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    const HRESULT hr = device.d3d()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_));
    if (FAILED(hr)) {
        if (error != nullptr)
            *error = "CreateDescriptorHeap (CPU views): " + hresult_text(hr);
        return false;
    }
    base_ = heap_->GetCPUDescriptorHandleForHeapStart();
    increment_ = device.d3d()->GetDescriptorHandleIncrementSize(type);
    capacity_ = capacity;
    next_ = 0;
    in_use_ = 0;
    free_list_.clear();
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE CpuDescriptorPool::allocate() {
    std::uint32_t index;
    if (!free_list_.empty()) {
        index = free_list_.back();
        free_list_.pop_back();
    } else if (next_ < capacity_) {
        index = next_++;
    } else {
        return D3D12_CPU_DESCRIPTOR_HANDLE{0};
    }
    ++in_use_;
    D3D12_CPU_DESCRIPTOR_HANDLE handle = base_;
    handle.ptr += static_cast<SIZE_T>(index) * increment_;
    return handle;
}

void CpuDescriptorPool::free(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    if (handle.ptr == 0 || increment_ == 0)
        return;
    const auto index = static_cast<std::uint32_t>((handle.ptr - base_.ptr) / increment_);
    free_list_.push_back(index);
    --in_use_;
}

// ---- SamplerHeap --------------------------------------------------------------------------------------------

bool SamplerHeap::create(Device& device, std::uint32_t max_anisotropy, std::string* error) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    desc.NumDescriptors = kSamplerCount;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    const HRESULT hr = device.d3d()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_));
    if (FAILED(hr)) {
        if (error != nullptr)
            *error = "CreateDescriptorHeap (samplers): " + hresult_text(hr);
        return false;
    }
    heap_->SetName(L"shadow.samplers");
    const std::uint32_t increment = device.d3d()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    const D3D12_TEXTURE_ADDRESS_MODE modes[3] = {D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                                 D3D12_TEXTURE_ADDRESS_MODE_MIRROR};
    const D3D12_FILTER filters[3] = {D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                                     D3D12_FILTER_ANISOTROPIC};
    for (std::uint32_t f = 0; f < 3; ++f) {
        for (std::uint32_t u = 0; u < 3; ++u) {
            for (std::uint32_t v = 0; v < 3; ++v) {
                D3D12_SAMPLER_DESC sampler{};
                sampler.Filter = filters[f];
                sampler.AddressU = modes[u];
                sampler.AddressV = modes[v];
                sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                sampler.MaxAnisotropy = f == 2 ? max_anisotropy : 1;
                sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
                sampler.MinLOD = 0.0f;
                sampler.MaxLOD = D3D12_FLOAT32_MAX;
                D3D12_CPU_DESCRIPTOR_HANDLE handle = heap_->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += static_cast<SIZE_T>(sampler_index(static_cast<Filter>(f), static_cast<Address>(u),
                                                                static_cast<Address>(v))) * increment;
                device.d3d()->CreateSampler(&sampler, handle);
            }
        }
    }
    return true;
}

}  // namespace shadow::render
