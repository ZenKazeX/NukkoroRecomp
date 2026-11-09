#include "render/skin_cache.h"

#include <algorithm>

#include <cstring>

#include "render/pipelines.h"

#include "render_cs_skin.h"

namespace shadow::render {

namespace {

template <class Uploader>
bool create_source(Device& device, BindlessHeap& heap, Uploader& uploader, const std::vector<SkinVertex>& vertices,
                   SkinSource* out, std::string* error) {
    *out = SkinSource{};
    const std::uint64_t bytes = vertices.size() * sizeof(SkinVertex);
    if (!create_buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON,
                       L"shadow.skin_source", &out->vertices, error))
        return false;
    if (!uploader.upload_buffer(out->vertices.resource.Get(), 0, vertices.data(), bytes, error))
        return false;
    out->srv = heap.allocate();
    if (out->srv == kNoDescriptor) {
        if (error != nullptr)
            *error = "bindless heap full";
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_UNKNOWN;
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Buffer.NumElements = static_cast<UINT>(vertices.size());
    view.Buffer.StructureByteStride = sizeof(SkinVertex);
    device.d3d()->CreateShaderResourceView(out->vertices.resource.Get(), &view, heap.cpu(out->srv));
    out->vertex_count = static_cast<std::uint32_t>(vertices.size());
    return true;
}

}  // namespace

bool create_skin_source(Device& device, BindlessHeap& heap, UploadBatch& batch, const std::vector<SkinVertex>& vertices,
                        SkinSource* out, std::string* error) {
    return create_source(device, heap, batch, vertices, out, error);
}

bool create_skin_source(Device& device, BindlessHeap& heap, UploadStream& uploads,
                        const std::vector<SkinVertex>& vertices, SkinSource* out, std::string* error) {
    return create_source(device, heap, uploads, vertices, out, error);
}

void retire(Device& device, SkinSource& source, BindlessHeap& heap) {
    if (source.srv != kNoDescriptor)
        heap.free(source.srv, device.mark());
    source.srv = kNoDescriptor;
    retire(device, source.vertices);
    source.vertex_count = 0;
}

bool create_morph_source(Device& device, BindlessHeap& heap, UploadStream& uploads,
                         const std::vector<MorphDelta>& deltas, std::uint32_t targets, MorphSource* out,
                         std::string* error) {
    *out = MorphSource{};
    const std::uint64_t bytes = deltas.size() * sizeof(MorphDelta);
    if (!create_buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON,
                       L"shadow.morph_deltas", &out->deltas, error))
        return false;
    if (!uploads.upload_buffer(out->deltas.resource.Get(), 0, deltas.data(), bytes, error))
        return false;
    out->srv = heap.allocate();
    if (out->srv == kNoDescriptor) {
        if (error != nullptr)
            *error = "bindless heap full";
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_UNKNOWN;
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Buffer.NumElements = static_cast<UINT>(deltas.size());
    view.Buffer.StructureByteStride = sizeof(MorphDelta);
    device.d3d()->CreateShaderResourceView(out->deltas.resource.Get(), &view, heap.cpu(out->srv));
    out->targets = targets;
    return true;
}

void retire(Device& device, MorphSource& source, BindlessHeap& heap) {
    if (source.srv != kNoDescriptor)
        heap.free(source.srv, device.mark());
    source.srv = kNoDescriptor;
    retire(device, source.deltas);
    source.targets = 0;
}

bool SkinCache::create(Device& device, BindlessHeap& heap, std::uint32_t vertices_per_frame,
                       std::uint32_t frames_in_flight, ID3D12RootSignature* root, std::string* error) {
    destroy();
    device_ = &device;
    heap_ = &heap;
    frames_in_flight_ = frames_in_flight;
    grows_ = 0;
    if (!make_buffer(vertices_per_frame, error))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.CS = {kRenderCSSkin, sizeof(kRenderCSSkin)};
    const HRESULT hr = device.d3d()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline_));
    if (FAILED(hr)) {
        if (error != nullptr)
            *error = "CreateComputePipelineState (skinning): " + hresult_text(hr);
        return false;
    }
    pipeline_->SetName(L"shadow.skinning");
    begin_frame(0);
    return true;
}

void SkinCache::destroy() {
    if (device_ == nullptr)
        return;
    if (uav_ != kNoDescriptor)
        heap_->free(uav_, device_->mark());
    uav_ = kNoDescriptor;
    if (buffer_)
        retire(*device_, buffer_);
    device_->retire(std::move(pipeline_));
    device_ = nullptr;
    heap_ = nullptr;
}

bool SkinCache::make_buffer(std::uint32_t vertices_per_frame, std::string* error) {
    GpuBuffer buffer;
    const std::uint64_t elements = static_cast<std::uint64_t>(vertices_per_frame) * frames_in_flight_;
    if (!create_buffer(*device_, elements * sizeof(SkinnedVertex), D3D12_HEAP_TYPE_DEFAULT,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, L"shadow.skin_cache",
                       &buffer, error))
        return false;
    const std::uint32_t uav = heap_->allocate();
    if (uav == kNoDescriptor) {
        if (error != nullptr)
            *error = "bindless heap full";
        retire(*device_, buffer);
        return false;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_UNKNOWN;
    view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements = static_cast<UINT>(elements);
    view.Buffer.StructureByteStride = sizeof(SkinnedVertex);
    device_->d3d()->CreateUnorderedAccessView(buffer.resource.Get(), nullptr, &view, heap_->cpu(uav));
    // The previous buffer and its view stay alive until the GPU has passed the frames that used them.
    if (uav_ != kNoDescriptor)
        heap_->free(uav_, device_->mark());
    if (buffer_)
        retire(*device_, buffer_);
    buffer_ = std::move(buffer);
    uav_ = uav;
    per_frame_ = vertices_per_frame;
    return true;
}

bool SkinCache::reserve(std::uint32_t vertices_per_frame, std::string* error) {
    if (vertices_per_frame <= per_frame_)
        return true;
    const std::uint64_t doubled = std::uint64_t{per_frame_} * 2u;
    const auto target = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        std::max<std::uint64_t>(vertices_per_frame, doubled), 0x7FFFFFFFu));
    if (!make_buffer(target, error))
        return false;
    ++grows_;
    return true;
}

void SkinCache::begin_frame(std::uint32_t slot) {
    region_begin_ = slot * per_frame_;
    used_ = 0;
}

std::uint32_t SkinCache::allocate(std::uint32_t count) {
    if (count > per_frame_ - used_) {
        ++overflows_;
        return 0xFFFFFFFFu;
    }
    const std::uint32_t first = region_begin_ + used_;
    used_ += count;
    return first;
}

void SkinCache::dispatch(ID3D12GraphicsCommandList* list, const SkinSource& source, D3D12_GPU_VIRTUAL_ADDRESS palette,
                         std::uint32_t first_element, const MorphSource* morph, const float* weights, bool skin) const {
    list->SetPipelineState(pipeline_.Get());
    // root0: source, destination, vertex count, first element; root1: the deltas, the target count (0: no morph),
    // skinning on / off; root2 / root3: the weights of targets 0..7 (shaders/skinning.hlsl).
    std::uint32_t constants[16] = {source.srv, uav_, source.vertex_count, first_element};
    const bool morphed = morph != nullptr && weights != nullptr && morph->targets != 0u;
    constants[4] = morphed ? morph->srv : kNoDescriptor;
    constants[5] = morphed ? morph->targets : 0u;
    constants[6] = skin ? 1u : 0u;
    if (morphed) {
        for (std::uint32_t t = 0; t < morph->targets && t < 8u; ++t)
            std::memcpy(&constants[8 + t], &weights[t], sizeof(float));
    }
    list->SetComputeRoot32BitConstants(kRootConstants, 16, constants, 0);
    list->SetComputeRootConstantBufferView(kRootPassConstants, palette);
    list->Dispatch((source.vertex_count + 63) / 64, 1, 1);
}

}  // namespace shadow::render
