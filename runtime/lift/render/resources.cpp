#include "render/resources.h"

#include "render/gpu_memory.h"
#include "render/init_trace.h"
#include "render/profile.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace shadow::render {

namespace {

constexpr std::uint64_t kStagingChunk = 16ull << 20;
constexpr std::uint64_t kOwnAllocationTexture = 1ull << 20;

bool fail(std::string* error, const std::string& what, HRESULT hr) {
    if (error != nullptr)
        *error = what + ": " + hresult_text(hr);
    return false;
}

}  // namespace

bool create_buffer(Device& device, std::uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                   D3D12_RESOURCE_STATES initial_state, const wchar_t* name, GpuBuffer* out, std::string* error) {
    init_trace::Scope trace("buffer", name, size);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<std::uint64_t>(size, 1);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = heap;
    // Large host-transfer buffers own their allocation. In a shared block an
    // upload leaves unused capacity, and a one-shot screenshot readback leaves
    // an 8 MiB empty block cached after its fence-complete retirement.
    if ((heap == D3D12_HEAP_TYPE_UPLOAD || heap == D3D12_HEAP_TYPE_READBACK) && desc.Width >= (1ull << 20))
        allocation.Flags = static_cast<D3D12MA::ALLOCATION_FLAGS>(allocation.Flags | D3D12MA::ALLOCATION_FLAG_COMMITTED);
    if (heap == D3D12_HEAP_TYPE_UPLOAD)
        initial_state = D3D12_RESOURCE_STATE_GENERIC_READ;
    else if (heap == D3D12_HEAP_TYPE_READBACK)
        initial_state = D3D12_RESOURCE_STATE_COPY_DEST;
    GpuBuffer buffer;
    const HRESULT hr = device.allocator()->CreateResource(&allocation, &desc, initial_state, nullptr,
                                                          &buffer.allocation, IID_PPV_ARGS(&buffer.resource));
    if (FAILED(hr))
        return fail(error, "CreateResource (buffer)", hr);
    buffer.resource->SetName(name);
    if (gpu_memory::enabled())
        buffer.allocation->SetName(name);
    buffer.size = size;
    buffer.heap = heap;
    *out = std::move(buffer);
    return true;
}

D3D12_RESOURCE_DESC texture_resource_desc(const TextureInfo& info) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = info.width;
    desc.Height = info.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = info.mip_levels;
    desc.Format = info.format;
    desc.SampleDesc.Count = std::max<std::uint32_t>(info.samples, 1);
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = info.flags;
    return desc;
}

bool allocate_target_memory(Device& device, std::uint64_t size, const wchar_t* name, Com<D3D12MA::Allocation>* out,
                            std::string* error) {
    init_trace::Scope trace("heap", name, size);
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    allocation.ExtraHeapFlags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
    // DIAGNOSTIC SHADOW_R2_TARGET_HEAP=all: a heap for every resource kind (resource heap tier 2 only).
    if (const char* v = std::getenv("SHADOW_R2_TARGET_HEAP"); v != nullptr && std::strcmp(v, "all") == 0)
        allocation.ExtraHeapFlags = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
    allocation.Flags = D3D12MA::ALLOCATION_FLAG_COMMITTED;  // its own heap: CreateAliasingResource needs one
    D3D12_RESOURCE_ALLOCATION_INFO info{};
    constexpr std::uint64_t kHeapGranularity = 64ull << 10;
    info.SizeInBytes = (size + kHeapGranularity - 1) & ~(kHeapGranularity - 1);
    info.Alignment = D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
    Com<D3D12MA::Allocation> memory;
    const HRESULT hr = device.allocator()->AllocateMemory(&allocation, &info, &memory);
    if (FAILED(hr))
        return fail(error, "AllocateMemory (render-target memory)", hr);
    if (gpu_memory::enabled())
        memory->SetName(name);
    *out = std::move(memory);
    return true;
}

bool create_placed_texture_2d(Device& device, const TextureInfo& info, D3D12MA::Allocation* memory, GpuTexture* out,
                              std::string* error) {
    const D3D12_RESOURCE_DESC desc = texture_resource_desc(info);
    init_trace::Scope trace("placed", info.name, static_cast<std::uint64_t>(desc.Width) * desc.Height);
    GpuTexture texture;
    const HRESULT hr = device.allocator()->CreateAliasingResource(memory, 0, &desc, info.initial_state, info.clear,
                                                                  IID_PPV_ARGS(&texture.resource));
    if (FAILED(hr))
        return fail(error, "CreateAliasingResource (texture)", hr);
    texture.resource->SetName(info.name);
    texture.desc = desc;
    *out = std::move(texture);
    return true;
}

bool create_texture_2d(Device& device, const TextureInfo& info, GpuTexture* out, std::string* error) {
    const D3D12_RESOURCE_DESC desc = texture_resource_desc(info);
    init_trace::Scope trace("texture", info.name, static_cast<std::uint64_t>(desc.Width) * desc.Height);
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    // Large textures (output-sized targets, the view history) own their allocation. Placed in a shared 64 MiB
    // block, a 30 MiB target left the rest of its block empty: 62 MiB of 168 MiB in blocks at 1440p, and every
    // default-heap byte is also charged to the process commit on the measured driver. At 720p a 3.8 MiB target alone
    // opened a new 32 MiB block. Asset textures and the small glare levels stay pooled.
    const D3D12_RESOURCE_ALLOCATION_INFO size = device.d3d()->GetResourceAllocationInfo(0, 1, &desc);
    if (size.SizeInBytes >= kOwnAllocationTexture)
        allocation.Flags = static_cast<D3D12MA::ALLOCATION_FLAGS>(allocation.Flags | D3D12MA::ALLOCATION_FLAG_COMMITTED);
    GpuTexture texture;
    const HRESULT hr = device.allocator()->CreateResource(&allocation, &desc, info.initial_state, info.clear,
                                                          &texture.allocation, IID_PPV_ARGS(&texture.resource));
    if (FAILED(hr))
        return fail(error, "CreateResource (texture)", hr);
    texture.resource->SetName(info.name);
    if (gpu_memory::enabled())
        texture.allocation->SetName(info.name);
    texture.desc = desc;
    *out = std::move(texture);
    return true;
}

bool create_texture_srv(Device& device, BindlessHeap& heap, GpuTexture& texture, DXGI_FORMAT view_format,
                        std::string* error) {
    const std::uint32_t index = heap.allocate();
    if (index == kNoDescriptor) {
        if (error != nullptr)
            *error = "bindless heap full";
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = view_format == DXGI_FORMAT_UNKNOWN ? texture.desc.Format : view_format;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (texture.desc.SampleDesc.Count > 1) {
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    } else {
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = texture.desc.MipLevels;
    }
    device.d3d()->CreateShaderResourceView(texture.resource.Get(), &view, heap.cpu(index));
    texture.srv = index;
    return true;
}

void retire(Device& device, GpuBuffer& buffer) {
    // The resource first, then the memory it was placed in.
    device.retire(std::move(buffer.resource));
    device.retire(Com<IUnknown>(buffer.allocation.Get()));
    buffer.allocation.Reset();
    buffer.size = 0;
}

void retire(Device& device, GpuTexture& texture, BindlessHeap* heap) {
    if (heap != nullptr && texture.srv != kNoDescriptor)
        heap->free(texture.srv, device.mark());
    texture.srv = kNoDescriptor;
    device.retire(std::move(texture.resource));
    device.retire(Com<IUnknown>(texture.allocation.Get()));
    texture.allocation.Reset();
}

// ---- UploadRing ---------------------------------------------------------------------------------------------

UploadRing::~UploadRing() {
    if (mapped_ != nullptr && buffer_.resource) {
        gpu_memory::note_unmap(mapped_);
        buffer_.resource->Unmap(0, nullptr);
    }
}

bool UploadRing::create(Device& device, std::uint64_t segment_bytes, std::uint32_t segments, std::string* error) {
    destroy(device);
    device_ = &device;
    spill_chunks_.assign(segments, {});
    spills_ = 0;
    if (!create_buffer(device, segment_bytes * segments, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                       D3D12_RESOURCE_STATE_GENERIC_READ, L"shadow.frame_ring", &buffer_, error))
        return false;
    const D3D12_RANGE nothing_read{0, 0};
    void* mapped = nullptr;
    const HRESULT hr = buffer_.resource->Map(0, &nothing_read, &mapped);
    if (FAILED(hr))
        return fail(error, "Map (frame ring)", hr);
    gpu_memory::note_map(L"shadow.frame_ring", mapped, segment_bytes * segments);
    mapped_ = static_cast<std::uint8_t*>(mapped);
    gpu_base_ = buffer_.gpu();
    segment_bytes_ = segment_bytes;
    segments_ = segments;
    begin(0);
    peak_ = 0;
    overflows_ = 0;
    return true;
}

void UploadRing::destroy(Device& device) {
    for (std::uint32_t segment = 0; segment < spill_chunks_.size(); ++segment)
        release_spills(segment);
    spill_chunks_.clear();
    if (mapped_ != nullptr && buffer_.resource) {
        gpu_memory::note_unmap(mapped_);
        buffer_.resource->Unmap(0, nullptr);
    }
    mapped_ = nullptr;
    if (buffer_)
        retire(device, buffer_);
    segment_bytes_ = 0;
    segments_ = 0;
}

void UploadRing::release_spills(std::uint32_t segment) {
    if (device_ == nullptr || segment >= spill_chunks_.size())
        return;
    for (Spill& chunk : spill_chunks_[segment]) {
        gpu_memory::note_unmap(chunk.cpu);
        if (chunk.buffer.resource)
            chunk.buffer.resource->Unmap(0, nullptr);
        retire(*device_, chunk.buffer);
    }
    spill_chunks_[segment].clear();
}

void UploadRing::begin(std::uint32_t segment) {
    segment_index_ = segment % std::max<std::uint32_t>(segments_, 1);
    segment_begin_ = static_cast<std::uint64_t>(segment_index_) * segment_bytes_;
    cursor_ = segment_begin_;
    spilled_bytes_ = 0;
    release_spills(segment_index_);  // the segment's previous frame is complete (FrameLoop waited for it)
}

UploadAllocation UploadRing::allocate(std::uint64_t bytes, std::uint64_t alignment) {
    const std::uint64_t start = align_up(cursor_, alignment);
    if (mapped_ != nullptr && start + bytes > segment_begin_ + segment_bytes_ && device_ != nullptr &&
        segment_index_ < spill_chunks_.size()) {
        // F-M1: the segment is full - the frame continues in a spill chunk of this segment.
        std::vector<Spill>& chunks = spill_chunks_[segment_index_];
        if (chunks.empty() || align_up(chunks.back().used, alignment) + bytes > chunks.back().buffer.size) {
            Spill chunk;
            const std::uint64_t size = std::max<std::uint64_t>(segment_bytes_, align_up(bytes + alignment, 1u << 16));
            std::string spill_error;
            void* mapped = nullptr;
            const D3D12_RANGE nothing_read{0, 0};
            if (!create_buffer(*device_, size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_GENERIC_READ, L"shadow.frame_ring_spill", &chunk.buffer,
                               &spill_error) ||
                FAILED(chunk.buffer.resource->Map(0, &nothing_read, &mapped))) {
                ++overflows_;
                return {};
            }
            chunk.cpu = static_cast<std::uint8_t*>(mapped);
            gpu_memory::note_map(L"shadow.frame_ring_spill", mapped, size);
            chunks.push_back(std::move(chunk));
            ++spills_;
        }
        Spill& chunk = chunks.back();
        const std::uint64_t at = align_up(chunk.used, alignment);
        chunk.used = at + bytes;
        spilled_bytes_ += bytes;
        peak_ = std::max(peak_, cursor_ - segment_begin_ + spilled_bytes_);
        UploadAllocation out;
        out.cpu = chunk.cpu + at;
        out.gpu = chunk.buffer.gpu() + at;
        out.buffer = chunk.buffer.resource.Get();
        out.offset = at;
        out.size = bytes;
        return out;
    }
    if (mapped_ == nullptr || start + bytes > segment_begin_ + segment_bytes_) {
        ++overflows_;
        return {};
    }
    cursor_ = start + bytes;
    peak_ = std::max(peak_, cursor_ - segment_begin_);
    UploadAllocation out;
    out.cpu = mapped_ + start;
    out.gpu = gpu_base_ + start;
    out.buffer = buffer_.resource.Get();
    out.offset = start;
    out.size = bytes;
    return out;
}

// ---- UploadBatch --------------------------------------------------------------------------------------------

UploadBatch::UploadBatch(Device& device) : device_(device) {}

UploadBatch::~UploadBatch() {
    if (recorded_)
        submit();
    for (GpuBuffer& chunk : chunks_) {
        if (chunk.resource)
            chunk.resource->Unmap(0, nullptr);
        retire(device_, chunk);
    }
    device_.retire(std::move(list_));
    device_.retire(std::move(allocator_));
}

bool UploadBatch::ensure_list(std::string* error) {
    if (submitted_) {
        if (error != nullptr)
            *error = "load batch already submitted";
        return false;
    }
    if (list_)
        return true;
    HRESULT hr = device_.d3d()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&allocator_));
    if (FAILED(hr))
        return fail(error, "CreateCommandAllocator (copy)", hr);
    hr = device_.d3d()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocator_.Get(), nullptr,
                                          IID_PPV_ARGS(&list_));
    if (FAILED(hr))
        return fail(error, "CreateCommandList (copy)", hr);
    list_->SetName(L"shadow.load_batch");
    return true;
}

bool UploadBatch::stage(std::uint64_t bytes, std::uint64_t alignment, std::uint8_t** cpu, ID3D12Resource** buffer,
                        std::uint64_t* offset, std::string* error) {
    std::uint64_t start = chunks_.empty() ? 0 : align_up(chunk_used_, alignment);
    if (chunks_.empty() || start + bytes > chunks_.back().size) {
        GpuBuffer chunk;
        const std::uint64_t size = std::max(kStagingChunk, align_up(bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT));
        if (!create_buffer(device_, size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ, L"shadow.load_staging", &chunk, error))
            return false;
        const D3D12_RANGE nothing_read{0, 0};
        void* mapped = nullptr;
        const HRESULT hr = chunk.resource->Map(0, &nothing_read, &mapped);
        if (FAILED(hr))
            return fail(error, "Map (load staging)", hr);
        chunks_.push_back(std::move(chunk));
        chunk_cpu_ = static_cast<std::uint8_t*>(mapped);
        start = 0;
    }
    chunk_used_ = start + bytes;
    staged_ += bytes;
    *cpu = chunk_cpu_ + start;
    *buffer = chunks_.back().resource.Get();
    *offset = start;
    return true;
}

bool UploadBatch::upload_buffer(ID3D12Resource* dst, std::uint64_t dst_offset, const void* data, std::uint64_t bytes,
                                std::string* error) {
    if (bytes == 0)
        return true;
    if (!ensure_list(error))
        return false;
    std::uint8_t* cpu = nullptr;
    ID3D12Resource* staging = nullptr;
    std::uint64_t offset = 0;
    if (!stage(bytes, 16, &cpu, &staging, &offset, error))
        return false;
    std::memcpy(cpu, data, static_cast<std::size_t>(bytes));
    list_->CopyBufferRegion(dst, dst_offset, staging, offset, bytes);
    recorded_ = true;
    return true;
}

bool UploadBatch::upload_texture(ID3D12Resource* dst, std::uint32_t first, std::uint32_t count,
                                 const SubresourceData* data, std::string* error) {
    if (count == 0)
        return true;
    if (!ensure_list(error))
        return false;
    const D3D12_RESOURCE_DESC desc = dst->GetDesc();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(count);
    std::vector<UINT> rows(count);
    std::vector<UINT64> row_bytes(count);
    UINT64 total = 0;
    device_.d3d()->GetCopyableFootprints(&desc, first, count, 0, layouts.data(), rows.data(), row_bytes.data(),
                                         &total);
    std::uint8_t* cpu = nullptr;
    ID3D12Resource* staging = nullptr;
    std::uint64_t offset = 0;
    if (!stage(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &cpu, &staging, &offset, error))
        return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout = layouts[i];
        const auto* src = static_cast<const std::uint8_t*>(data[i].data);
        std::uint8_t* dst_rows = cpu + layout.Offset;
        for (UINT slice = 0; slice < layout.Footprint.Depth; ++slice) {
            for (UINT row = 0; row < rows[i]; ++row) {
                std::memcpy(dst_rows + (static_cast<std::uint64_t>(slice) * rows[i] + row) * layout.Footprint.RowPitch,
                            src + (static_cast<std::uint64_t>(slice) * rows[i] + row) * data[i].row_pitch,
                            static_cast<std::size_t>(row_bytes[i]));
            }
        }
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = dst;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.SubresourceIndex = first + i;
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = staging;
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = layout;
        from.PlacedFootprint.Offset += offset;
        list_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    recorded_ = true;
    return true;
}

std::uint64_t UploadBatch::submit() {
    if (!recorded_)
        return 0;
    list_->Close();
    ID3D12CommandList* lists[] = {list_.Get()};
    const std::uint64_t value = device_.copy().submit(lists, 1);
    recorded_ = false;
    submitted_ = true;
    // The staging chunks, the list and its allocator stay alive until the copy queue passes `value` (retired by
    // the destructor with the mark taken after this submission). A batch is recorded once.
    return value;
}

void UploadBatch::make_visible(std::uint64_t value) {
    device_.direct().wait_for(device_.copy(), value);
}

// ---- UploadStream ------------------------------------------------------------------------------------------------

UploadStream::~UploadStream() {
    destroy();
}

bool UploadStream::create(Device& device, std::uint64_t ring_bytes, std::string* error) {
    destroy();
    device_ = &device;
    // A multiple of 64 KiB keeps every ring offset's alignment (texture placements need 512 bytes).
    ring_size_ = align_up(std::max<std::uint64_t>(ring_bytes, 1u << 16), 1u << 16);
    if (!create_buffer(device, ring_size_, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                       D3D12_RESOURCE_STATE_GENERIC_READ, L"shadow.upload_stream", &ring_, error))
        return false;
    const D3D12_RANGE nothing_read{0, 0};
    void* mapped = nullptr;
    const HRESULT hr = ring_.resource->Map(0, &nothing_read, &mapped);
    if (FAILED(hr))
        return fail(error, "Map (upload stream)", hr);
    gpu_memory::note_map(L"shadow.upload_stream", mapped, ring_size_);
    ring_cpu_ = static_cast<std::uint8_t*>(mapped);
    return true;
}

void UploadStream::destroy() {
    if (device_ == nullptr)
        return;
    flush();
    if (last_fence_ != 0)
        device_->copy().wait(last_fence_);
    in_flight_.clear();
    spare_allocators_.clear();
    if (list_ && open_)
        list_->Close();
    list_.Reset();
    allocator_.Reset();
    if (ring_.resource) {
        gpu_memory::note_unmap(ring_cpu_);
        ring_.resource->Unmap(0, nullptr);
    }
    ring_ = GpuBuffer{};
    ring_cpu_ = nullptr;
    head_ = tail_ = 0;
    open_ = recorded_ = false;
    device_ = nullptr;
}

bool UploadStream::ensure_list(std::string* error) {
    if (open_)
        return true;
    reclaim(false);  // spare allocators come back from finished submissions
    HRESULT hr = S_OK;
    if (!spare_allocators_.empty()) {
        allocator_ = std::move(spare_allocators_.back());
        spare_allocators_.pop_back();
        hr = allocator_->Reset();
    } else {
        hr = device_->d3d()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&allocator_));
        ++allocators_created_;
    }
    publish();
    if (FAILED(hr))
        return fail(error, "command allocator (upload stream)", hr);
    if (!list_) {
        hr = device_->d3d()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocator_.Get(), nullptr,
                                               IID_PPV_ARGS(&list_));
        if (SUCCEEDED(hr))
            list_->SetName(L"shadow.upload_stream");
    } else {
        hr = list_->Reset(allocator_.Get(), nullptr);
    }
    if (FAILED(hr))
        return fail(error, "command list (upload stream)", hr);
    open_ = true;
    return true;
}

void UploadStream::reclaim(bool wait) {
    while (!in_flight_.empty()) {
        InFlight& oldest = in_flight_.front();
        if (!device_->copy().reached(oldest.fence)) {
            if (!wait)
                break;
            GEKKO_PROFILE_ZONE("res/upload-wait");
            init_trace::Scope trace("upload-wait", L"ring");
            device_->copy().wait(oldest.fence);
            wait = false;
        }
        tail_ = oldest.ring_end;
        spare_allocators_.push_back(std::move(oldest.allocator));
        in_flight_.pop_front();
    }
}

bool UploadStream::reserve(std::uint64_t bytes, std::uint64_t alignment, std::uint8_t** cpu, ID3D12Resource** buffer,
                           std::uint64_t* offset, std::string* error) {
    if (bytes > ring_size_) {
        GpuBuffer staging;
        if (!create_buffer(*device_, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ, L"shadow.upload_dedicated", &staging, error))
            return false;
        const D3D12_RANGE nothing_read{0, 0};
        void* mapped = nullptr;
        const HRESULT hr = staging.resource->Map(0, &nothing_read, &mapped);
        if (FAILED(hr))
            return fail(error, "Map (dedicated upload)", hr);
        gpu_memory::note_map(L"shadow.upload_dedicated", mapped, bytes);
        *cpu = static_cast<std::uint8_t*>(mapped);
        *buffer = staging.resource.Get();
        *offset = 0;
        dedicated_.push_back(std::move(staging));
        dedicated_cpu_.push_back(mapped);
        ++stats_.dedicated;
        stats_.staged_bytes += bytes;
        return true;
    }
    for (;;) {
        std::uint64_t start = align_up(head_, alignment);
        const std::uint64_t at = start % ring_size_;
        if (at + bytes > ring_size_)
            start += ring_size_ - at;  // never straddle the end: continue at offset 0 of the next lap
        if (start + bytes - tail_ <= ring_size_) {
            head_ = start + bytes;
            *cpu = ring_cpu_ + start % ring_size_;
            *buffer = ring_.resource.Get();
            *offset = start % ring_size_;
            stats_.staged_bytes += bytes;
            return true;
        }
        // Full: what is recorded goes to the GPU, then the oldest submission's space comes back.
        if (recorded_)
            flush();
        if (in_flight_.empty()) {
            // Nothing reads the ring any more: start a new lap at offset 0.
            head_ = tail_ = align_up(head_, ring_size_);
            continue;
        }
        ++stats_.ring_waits;
        reclaim(true);
    }
}

bool UploadStream::upload_buffer(ID3D12Resource* dst, std::uint64_t dst_offset, const void* data, std::uint64_t bytes,
                                 std::string* error) {
    if (bytes == 0)
        return true;
    std::uint8_t* cpu = nullptr;
    ID3D12Resource* staging = nullptr;
    std::uint64_t offset = 0;
    if (!reserve(bytes, 16, &cpu, &staging, &offset, error))
        return false;
    std::memcpy(cpu, data, static_cast<std::size_t>(bytes));
    if (!ensure_list(error))
        return false;
    list_->CopyBufferRegion(dst, dst_offset, staging, offset, bytes);
    recorded_ = true;
    return true;
}

bool UploadStream::upload_texture(ID3D12Resource* dst, std::uint32_t first, std::uint32_t count,
                                  const SubresourceData* data, std::string* error) {
    if (count == 0)
        return true;
    const D3D12_RESOURCE_DESC desc = dst->GetDesc();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(count);
    std::vector<UINT> rows(count);
    std::vector<UINT64> row_bytes(count);
    UINT64 total = 0;
    device_->d3d()->GetCopyableFootprints(&desc, first, count, 0, layouts.data(), rows.data(), row_bytes.data(),
                                          &total);
    std::uint8_t* cpu = nullptr;
    ID3D12Resource* staging = nullptr;
    std::uint64_t offset = 0;
    if (!reserve(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &cpu, &staging, &offset, error))
        return false;
    if (!ensure_list(error))
        return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout = layouts[i];
        const auto* src = static_cast<const std::uint8_t*>(data[i].data);
        std::uint8_t* dst_rows = cpu + layout.Offset;
        for (UINT slice = 0; slice < layout.Footprint.Depth; ++slice) {
            for (UINT row = 0; row < rows[i]; ++row) {
                std::memcpy(dst_rows + (static_cast<std::uint64_t>(slice) * rows[i] + row) * layout.Footprint.RowPitch,
                            src + (static_cast<std::uint64_t>(slice) * rows[i] + row) * data[i].row_pitch,
                            static_cast<std::size_t>(row_bytes[i]));
            }
        }
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = dst;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.SubresourceIndex = first + i;
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = staging;
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = layout;
        from.PlacedFootprint.Offset += offset;
        list_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    recorded_ = true;
    return true;
}

std::uint64_t UploadStream::flush() {
    if (!recorded_)
        return last_fence_;
    GEKKO_PROFILE_ZONE("res/upload-flush");
    list_->Close();
    open_ = false;
    ID3D12CommandList* lists[] = {list_.Get()};
    last_fence_ = device_->copy().submit(lists, 1);
    in_flight_.push_back(InFlight{last_fence_, head_, std::move(allocator_)});
    // Retired after the submission, so their mark covers it.
    for (GpuBuffer& staging : dedicated_) {
        staging.resource->Unmap(0, nullptr);
        retire(*device_, staging);
    }
    for (void* cpu : dedicated_cpu_)
        gpu_memory::note_unmap(cpu);
    dedicated_.clear();
    dedicated_cpu_.clear();
    recorded_ = false;
    ++stats_.submissions;
    publish();
    return last_fence_;
}

void UploadStream::publish() noexcept {
    published_.submissions.store(stats_.submissions, std::memory_order_relaxed);
    published_.staged_bytes.store(stats_.staged_bytes, std::memory_order_relaxed);
    published_.ring_waits.store(stats_.ring_waits, std::memory_order_relaxed);
    published_.dedicated.store(stats_.dedicated, std::memory_order_relaxed);
    published_.allocators_created.store(allocators_created_, std::memory_order_relaxed);
    published_.allocators_spare.store(spare_allocators_.size(), std::memory_order_relaxed);
    published_.in_flight.store(in_flight_.size(), std::memory_order_relaxed);
}

UploadStream::Counts UploadStream::counts() const noexcept {
    Counts c;
    c.submissions = published_.submissions.load(std::memory_order_relaxed);
    c.staged_bytes = published_.staged_bytes.load(std::memory_order_relaxed);
    c.ring_waits = published_.ring_waits.load(std::memory_order_relaxed);
    c.dedicated = published_.dedicated.load(std::memory_order_relaxed);
    c.allocators_created = published_.allocators_created.load(std::memory_order_relaxed);
    c.allocators_spare = published_.allocators_spare.load(std::memory_order_relaxed);
    c.in_flight = published_.in_flight.load(std::memory_order_relaxed);
    return c;
}

}  // namespace shadow::render
