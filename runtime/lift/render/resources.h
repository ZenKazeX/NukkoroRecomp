#pragma once

// GPU memory the renderer owns: buffers and textures placed by D3D12 Memory Allocator, the per-frame upload
// ring, and load batches on the copy queue (docs/lift-20261006/R_DESIGN.md 2.6).
//
//   * A resource is created once for its lifetime scope and released through `Device::retire`, which keeps it
//     alive until the GPU has passed its last use.
//   * Per-frame data (draw records, bone palettes, particle and sprite records, pass constants) is written
//     straight into a persistently mapped upload-heap segment that belongs to one frame in flight. The segment
//     is sized for the whole frame; running out is reported and counted, never waited on.
//   * Load-time data (meshes, textures) is copied by the copy queue in batches: the batch stages the bytes,
//     records the copies, signals the copy fence, and the direct queue waits on that value before first use.
//     Destinations start in the COMMON state, so neither queue needs a barrier for them: the copy queue promotes
//     them to COPY_DEST, they decay back when the batch completes, and the direct queue promotes them to the
//     read state a draw needs.

#include <atomic>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "render/descriptors.h"
#include "render/gpu_device.h"

#include <D3D12MemAlloc.h>

namespace shadow::render {

struct GpuBuffer {
    Com<ID3D12Resource> resource;
    Com<D3D12MA::Allocation> allocation;
    std::uint64_t size = 0;
    D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT;

    explicit operator bool() const noexcept { return resource != nullptr; }
    D3D12_GPU_VIRTUAL_ADDRESS gpu() const { return resource->GetGPUVirtualAddress(); }
};

struct GpuTexture {
    Com<ID3D12Resource> resource;
    Com<D3D12MA::Allocation> allocation;
    D3D12_RESOURCE_DESC desc{};
    std::uint32_t srv = kNoDescriptor;  // bindless index, when one was created

    explicit operator bool() const noexcept { return resource != nullptr; }
};

bool create_buffer(Device& device, std::uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                   D3D12_RESOURCE_STATES initial_state, const wchar_t* name, GpuBuffer* out, std::string* error);

struct TextureInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t mip_levels = 1;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::uint32_t samples = 1;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON;
    const D3D12_CLEAR_VALUE* clear = nullptr;  // optimized clear value for render targets / depth
    const wchar_t* name = L"shadow.texture";
};

bool create_texture_2d(Device& device, const TextureInfo& info, GpuTexture* out, std::string* error);

// The resource description create_texture_2d makes for `info`.
D3D12_RESOURCE_DESC texture_resource_desc(const TextureInfo& info);

// Render-target memory that several textures can be placed in (memory aliasing): its own heap of `size` bytes, for
// render-target and depth-stencil textures only (legal on every resource heap tier), aligned for multisampled ones,
// so a texture of any sample count fits at offset 0.
bool allocate_target_memory(Device& device, std::uint64_t size, const wchar_t* name, Com<D3D12MA::Allocation>* out,
                            std::string* error);
// A render-target or depth texture placed at offset 0 of `memory`. It owns no memory (`allocation` stays empty): the
// memory outlives it and is retired separately. Like any placed resource sharing memory, it must be activated (an
// aliasing barrier, then a discard, clear or whole copy) before each use that follows another resource's use.
bool create_placed_texture_2d(Device& device, const TextureInfo& info, D3D12MA::Allocation* memory, GpuTexture* out,
                              std::string* error);

// Creates a shader-resource view of the whole texture at a new persistent bindless index (stored in `srv`).
// `view_format` overrides the resource format (depth formats need their readable twin).
bool create_texture_srv(Device& device, BindlessHeap& heap, GpuTexture& texture, DXGI_FORMAT view_format,
                        std::string* error);

// Hands the resource (and its bindless index) to deferred release.
void retire(Device& device, GpuBuffer& buffer);
void retire(Device& device, GpuTexture& texture, BindlessHeap* heap);

// ---- per-frame upload ring ----------------------------------------------------------------------------------

struct UploadAllocation {
    void* cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    ID3D12Resource* buffer = nullptr;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;

    explicit operator bool() const noexcept { return cpu != nullptr; }
};

class UploadRing {
public:
    UploadRing() = default;
    UploadRing(const UploadRing&) = delete;
    UploadRing& operator=(const UploadRing&) = delete;
    ~UploadRing();

    bool create(Device& device, std::uint64_t segment_bytes, std::uint32_t segments, std::string* error);
    void destroy(Device& device);

    // Starts filling `segment` (its previous frame must be complete): that segment's spill chunks are released.
    void begin(std::uint32_t segment);
    // F-M1 (HW_REPORT P / R): the segments are sized to what frames use (MiB, not 8), and a frame that needs more is
    // never short: what does not fit spills into an extra upload chunk of this segment, released when the segment is
    // next begun (counted: spill_count). Empty only when that chunk cannot be made (`overflow_count`).
    UploadAllocation allocate(std::uint64_t bytes, std::uint64_t alignment);

    std::uint64_t segment_bytes() const noexcept { return segment_bytes_; }
    std::uint64_t used() const noexcept { return cursor_ - segment_begin_ + spilled_bytes_; }
    std::uint64_t peak() const noexcept { return peak_; }
    std::uint64_t overflow_count() const noexcept { return overflows_; }
    std::uint64_t spill_count() const noexcept { return spills_; }

private:
    struct Spill {
        GpuBuffer buffer;
        std::uint8_t* cpu = nullptr;
        std::uint64_t used = 0;
    };
    void release_spills(std::uint32_t segment);
    Device* device_ = nullptr;
    std::vector<std::vector<Spill>> spill_chunks_;  // by segment
    std::uint32_t segment_index_ = 0;
    std::uint64_t spilled_bytes_ = 0;
    std::uint64_t spills_ = 0;
    GpuBuffer buffer_;
    std::uint8_t* mapped_ = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_base_ = 0;
    std::uint64_t segment_bytes_ = 0;
    std::uint32_t segments_ = 0;
    std::uint64_t segment_begin_ = 0;
    std::uint64_t cursor_ = 0;
    std::uint64_t peak_ = 0;
    std::uint64_t overflows_ = 0;
};

// ---- load batches on the copy queue -------------------------------------------------------------------------

// One subresource's source bytes: `rows` rows of `row_bytes` each (block rows for block-compressed formats),
// `row_pitch` apart in memory.
struct SubresourceData {
    const void* data = nullptr;
    std::uint64_t row_pitch = 0;
};

class UploadBatch {
public:
    explicit UploadBatch(Device& device);
    UploadBatch(const UploadBatch&) = delete;
    UploadBatch& operator=(const UploadBatch&) = delete;
    ~UploadBatch();

    // `dst` must be in the COMMON state (as created) and not in use by the GPU.
    bool upload_buffer(ID3D12Resource* dst, std::uint64_t dst_offset, const void* data, std::uint64_t bytes,
                       std::string* error);
    // Fills subresources [first, first + count) of a texture (mip-major order for 2D textures).
    bool upload_texture(ID3D12Resource* dst, std::uint32_t first, std::uint32_t count, const SubresourceData* data,
                        std::string* error);

    // Executes the batch on the copy queue. Returns the copy fence value after which the data is in place (0 if
    // nothing was recorded). `make_visible` orders the direct queue after it.
    std::uint64_t submit();
    void make_visible(std::uint64_t value);

    std::uint64_t staged_bytes() const noexcept { return staged_; }

private:
    bool ensure_list(std::string* error);
    bool stage(std::uint64_t bytes, std::uint64_t alignment, std::uint8_t** cpu, ID3D12Resource** buffer,
               std::uint64_t* offset, std::string* error);

    Device& device_;
    Com<ID3D12CommandAllocator> allocator_;
    Com<ID3D12GraphicsCommandList> list_;
    std::vector<GpuBuffer> chunks_;
    std::uint8_t* chunk_cpu_ = nullptr;
    std::uint64_t chunk_used_ = 0;
    std::uint64_t staged_ = 0;
    bool recorded_ = false;
    bool submitted_ = false;
};

// ---- the upload stream: resources created one at a time while the game runs ----------------------------------------

// The game creates its textures and meshes one at a time while it runs (a texture dictionary read, a world sector's
// first draw), from its own thread. The stream stages each one in a persistent upload ring and records its copies
// into one open copy list; `flush` submits everything recorded since the last flush as one execution. The render
// thread flushes before each frame and makes the direct queue wait on the returned value, so a resource is visible to
// the first frame recorded after its creation and the game never waits for the GPU. A full ring submits the open list
// and waits for the oldest submission still reading the ring; an upload larger than the ring gets a staging buffer of
// its own. Not thread-safe: the renderer calls it under its lock.
class UploadStream {
public:
    UploadStream() = default;
    UploadStream(const UploadStream&) = delete;
    UploadStream& operator=(const UploadStream&) = delete;
    ~UploadStream();

    bool create(Device& device, std::uint64_t ring_bytes, std::string* error);
    void destroy();

    // `dst` must be in the COMMON state (as created) and not in use by the GPU.
    bool upload_buffer(ID3D12Resource* dst, std::uint64_t dst_offset, const void* data, std::uint64_t bytes,
                       std::string* error);
    bool upload_texture(ID3D12Resource* dst, std::uint32_t first, std::uint32_t count, const SubresourceData* data,
                        std::string* error);

    // Submits the copies recorded since the last flush. Returns the copy fence value after which every upload
    // recorded so far is in place (0 before the first one).
    std::uint64_t flush();
    bool pending() const noexcept { return recorded_; }

    struct Stats {
        std::uint64_t submissions = 0;
        std::uint64_t staged_bytes = 0;
        std::uint64_t ring_waits = 0;  // the ring was full: the CPU waited for the copy queue
        std::uint64_t dedicated = 0;   // uploads larger than the ring
    };
    const Stats& stats() const noexcept { return stats_; }

    // Any thread (the GPU memory trace, gpu_memory.h): the counters as last published by the stream's own thread,
    // with its copy command allocators - created, spare (kept for reuse), in flight (HW_REPORT L, F-M3: counted only).
    struct Counts {
        std::uint64_t submissions = 0;
        std::uint64_t staged_bytes = 0;
        std::uint64_t ring_waits = 0;
        std::uint64_t dedicated = 0;
        std::uint64_t allocators_created = 0;
        std::uint64_t allocators_spare = 0;
        std::uint64_t in_flight = 0;
    };
    Counts counts() const noexcept;

private:
    bool ensure_list(std::string* error);
    bool reserve(std::uint64_t bytes, std::uint64_t alignment, std::uint8_t** cpu, ID3D12Resource** buffer,
                 std::uint64_t* offset, std::string* error);
    // Returns the ring space of finished submissions; with `wait`, first waits for the oldest one.
    void reclaim(bool wait);
    // Publishes the counters for counts().
    void publish() noexcept;

    struct InFlight {
        std::uint64_t fence = 0;
        std::uint64_t ring_end = 0;  // head_ when it was submitted
        Com<ID3D12CommandAllocator> allocator;
    };

    Device* device_ = nullptr;
    GpuBuffer ring_;
    std::uint8_t* ring_cpu_ = nullptr;
    std::uint64_t ring_size_ = 0;
    // Positions in an unbounded byte sequence laid over the ring (offset = position % ring_size_): [tail_, head_) may
    // still be read by the copy queue.
    std::uint64_t head_ = 0;
    std::uint64_t tail_ = 0;
    std::deque<InFlight> in_flight_;
    std::vector<Com<ID3D12CommandAllocator>> spare_allocators_;
    Com<ID3D12CommandAllocator> allocator_;
    Com<ID3D12GraphicsCommandList> list_;
    std::vector<GpuBuffer> dedicated_;  // staging of the open list's oversized uploads
    std::vector<void*> dedicated_cpu_;  // ... and their CPU mappings (the GPU memory trace's unmap records)
    bool open_ = false;
    bool recorded_ = false;
    std::uint64_t last_fence_ = 0;
    Stats stats_;
    std::uint64_t allocators_created_ = 0;
    struct Published {
        std::atomic<std::uint64_t> submissions{0}, staged_bytes{0}, ring_waits{0}, dedicated{0},
            allocators_created{0}, allocators_spare{0}, in_flight{0};
    } published_;
};

inline std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return alignment <= 1 ? value : (value + alignment - 1) / alignment * alignment;
}

}  // namespace shadow::render
