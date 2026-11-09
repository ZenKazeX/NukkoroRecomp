#pragma once

// The skin cache (docs/lift-20261006/R_DESIGN.md 2.4, 2.10): skinned meshes are skinned once per frame by a compute
// pass into a per-frame region of one buffer; every pass that draws a skinned mesh reads that region as its vertex
// stream. Chosen from the census: a skin is drawn 5.11 times per frame on average (base meshes plus environment and
// dual passes), and second passes over a mesh need bit-identical positions.
//
// Skinned meshes upload their bind pose once (`create_skin_source`); each frame the caller writes the palette into
// the upload ring, allocates output space and records the dispatch; the frame graph then orders the dispatch's
// writes before the draws' reads (Access::UnorderedAccess -> Access::VertexRead).

#include <cstdint>
#include <string>
#include <vector>

#include "render/descriptors.h"
#include "render/gpu_device.h"
#include "render/resources.h"

namespace shadow::render {

// One bind-pose vertex as the skinning pass reads it (matches SkinVertex in shaders/skinning.hlsl).
struct SkinVertex {
    float position[3];
    float normal[3];
    float weights[4];
    std::uint32_t indices;  // four 8-bit bone indices, the first in the lowest byte
};
static_assert(sizeof(SkinVertex) == 44, "SkinVertex layout");

// One skinned vertex as the draws read it (matches SkinnedVertex in shaders/skinning.hlsl).
struct SkinnedVertex {
    float position[3];
    float normal[3];
};
static_assert(sizeof(SkinnedVertex) == 24, "SkinnedVertex layout");

// A palette entry: p' = M * [p, 1], rows of a 3x4 matrix (matches the HLSL row_major float3x4).
struct BoneMatrix {
    float rows[3][4];
};
inline constexpr std::uint32_t kMaxPaletteBones = 256;

struct SkinSource {
    GpuBuffer vertices;                // SkinVertex[count], read through a bindless index
    std::uint32_t srv = kNoDescriptor;
    std::uint32_t vertex_count = 0;
};

// One vertex's offset in one delta-morph target (matches MorphDelta in shaders/skinning.hlsl).
struct MorphDelta {
    float position[3];
    float normal[3];
};
static_assert(sizeof(MorphDelta) == 24, "MorphDelta layout");

// A delta-morphed mesh's targets, uploaded once: target t's offset of vertex v is element t * vertex_count + v. The
// pass blends them onto the mesh's base vertices (its SkinSource) with each draw's weights.
struct MorphSource {
    GpuBuffer deltas;
    std::uint32_t srv = kNoDescriptor;
    std::uint32_t targets = 0;  // 0: not morphed
};

bool create_skin_source(Device& device, BindlessHeap& heap, UploadBatch& batch,
                        const std::vector<SkinVertex>& vertices, SkinSource* out, std::string* error);
// The same, through the upload stream (a skinned mesh created while the game runs).
bool create_skin_source(Device& device, BindlessHeap& heap, UploadStream& uploads,
                        const std::vector<SkinVertex>& vertices, SkinSource* out, std::string* error);
void retire(Device& device, SkinSource& source, BindlessHeap& heap);
// A morphed mesh's targets (`deltas` target-major, `targets` x the mesh's vertex count), through the upload stream.
bool create_morph_source(Device& device, BindlessHeap& heap, UploadStream& uploads,
                         const std::vector<MorphDelta>& deltas, std::uint32_t targets, MorphSource* out,
                         std::string* error);
void retire(Device& device, MorphSource& source, BindlessHeap& heap);

class SkinCache {
public:
    bool create(Device& device, BindlessHeap& heap, std::uint32_t vertices_per_frame, std::uint32_t frames_in_flight,
                ID3D12RootSignature* root, std::string* error);
    void destroy();

    // G12 (HW_REPORT P / R): the frame's deformed vertices fit before begin_frame - the cache grows (at least
    // doubling) when they would not, so a frame is never short of room. The old buffer is retired (frames in flight
    // keep reading it). False only when the larger buffer cannot be made (the old one stays).
    bool reserve(std::uint32_t vertices_per_frame, std::string* error);
    std::uint32_t vertices_per_frame() const noexcept { return per_frame_; }
    std::uint64_t grow_count() const noexcept { return grows_; }
    // Starts the frame slot's region.
    void begin_frame(std::uint32_t slot);
    // First element of `count` skinned vertices in this frame's region; UINT32_MAX when the region is full (counted).
    std::uint32_t allocate(std::uint32_t count);

    // Records one mesh's deformation: the delta targets of `morph` (when given) blended onto the source's vertices
    // with `weights` (morph->targets of them), then - when `skin` - the vertices posed by `palette`, the GPU address of
    // kMaxPaletteBones BoneMatrix entries in the upload ring (256-byte aligned; any valid block when not skinning).
    // The caller's frame graph declares the cache buffer as written (UnorderedAccess) by the pass that records this.
    void dispatch(ID3D12GraphicsCommandList* list, const SkinSource& source, D3D12_GPU_VIRTUAL_ADDRESS palette,
                  std::uint32_t first_element, const MorphSource* morph = nullptr, const float* weights = nullptr,
                  bool skin = true) const;

    ID3D12Resource* buffer() const noexcept { return buffer_.resource.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS element_address(std::uint32_t element) const {
        return buffer_.gpu() + static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(element) * sizeof(SkinnedVertex);
    }
    std::uint64_t overflow_count() const noexcept { return overflows_; }

private:
    Device* device_ = nullptr;
    BindlessHeap* heap_ = nullptr;
    GpuBuffer buffer_;
    std::uint32_t uav_ = kNoDescriptor;
    Com<ID3D12PipelineState> pipeline_;
    std::uint32_t per_frame_ = 0;
    std::uint32_t frames_in_flight_ = 0;
    std::uint64_t grows_ = 0;
    bool make_buffer(std::uint32_t vertices_per_frame, std::string* error);
    std::uint32_t region_begin_ = 0;
    std::uint32_t used_ = 0;
    std::uint64_t overflows_ = 0;
};

}  // namespace shadow::render
