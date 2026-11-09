#pragma once

// The renderer's one root signature and pipeline-state creation (docs/lift-20261006/R_DESIGN.md 2.3, 2.6).
//
// Every shader sees the same root: 16 root constants at b0 (per-draw indices into the frame's record buffer and
// the bindless heaps), a root constant buffer at b1 (the pass) and one at b2 (the frame). Textures, buffers and
// samplers are reached through ResourceDescriptorHeap / SamplerDescriptorHeap, so the root never changes between
// draws and pipeline states differ only in shaders and fixed-function state.

#include <cstdint>
#include <string>

#include "render/gpu_device.h"

namespace shadow::render {

inline constexpr std::uint32_t kRootConstantCount = 16;

enum RootSlot : std::uint32_t {
    kRootConstants = 0,       // b0, 16 x 32-bit
    kRootPassConstants = 1,   // b1, root CBV
    kRootFrameConstants = 2,  // b2, root CBV
};

bool create_root_signature(Device& device, Com<ID3D12RootSignature>* out, std::string* error);

struct GraphicsPipelineInfo {
    D3D12_SHADER_BYTECODE vs{};
    D3D12_SHADER_BYTECODE ps{};
    DXGI_FORMAT color_format = DXGI_FORMAT_R8G8B8A8_UNORM;  // UNKNOWN for depth-only pipelines
    DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t samples = 1;
    D3D12_BLEND_DESC blend = default_blend();
    D3D12_RASTERIZER_DESC raster = default_raster();
    D3D12_DEPTH_STENCIL_DESC depth = no_depth();
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    const D3D12_INPUT_ELEMENT_DESC* inputs = nullptr;
    std::uint32_t input_count = 0;
    const wchar_t* name = L"shadow.pipeline";

    static D3D12_BLEND_DESC default_blend();
    static D3D12_RASTERIZER_DESC default_raster();
    static D3D12_DEPTH_STENCIL_DESC no_depth();
};

bool create_graphics_pipeline(Device& device, ID3D12RootSignature* root, const GraphicsPipelineInfo& info,
                              Com<ID3D12PipelineState>* out, std::string* error);

}  // namespace shadow::render
