#include "render/pipelines.h"

namespace shadow::render {

namespace {

bool fail(std::string* error, const std::string& what, HRESULT hr) {
    if (error != nullptr)
        *error = what + ": " + hresult_text(hr);
    return false;
}

}  // namespace

bool create_root_signature(Device& device, Com<ID3D12RootSignature>* out, std::string* error) {
    D3D12_ROOT_PARAMETER1 parameters[3]{};
    parameters[kRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[kRootConstants].Constants.ShaderRegister = 0;
    parameters[kRootConstants].Constants.Num32BitValues = kRootConstantCount;
    parameters[kRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (std::uint32_t slot : {static_cast<std::uint32_t>(kRootPassConstants),
                               static_cast<std::uint32_t>(kRootFrameConstants)}) {
        parameters[slot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[slot].Descriptor.ShaderRegister = slot;
        parameters[slot].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
        parameters[slot].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = 3;
    desc.Desc_1_1.pParameters = parameters;
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                          D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                          D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    Com<ID3DBlob> blob;
    Com<ID3DBlob> messages;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &messages);
    if (FAILED(hr)) {
        std::string text = "D3D12SerializeVersionedRootSignature";
        if (messages)
            text += std::string(" (") + static_cast<const char*>(messages->GetBufferPointer()) + ")";
        return fail(error, text, hr);
    }
    hr = device.d3d()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&*out));
    if (FAILED(hr))
        return fail(error, "CreateRootSignature", hr);
    (*out)->SetName(L"shadow.root");
    return true;
}

D3D12_BLEND_DESC GraphicsPipelineInfo::default_blend() {
    D3D12_BLEND_DESC blend{};
    for (D3D12_RENDER_TARGET_BLEND_DESC& target : blend.RenderTarget) {
        target.SrcBlend = D3D12_BLEND_ONE;
        target.DestBlend = D3D12_BLEND_ZERO;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = D3D12_BLEND_ZERO;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    return blend;
}

D3D12_RASTERIZER_DESC GraphicsPipelineInfo::default_raster() {
    D3D12_RASTERIZER_DESC raster{};
    raster.FillMode = D3D12_FILL_MODE_SOLID;
    raster.CullMode = D3D12_CULL_MODE_NONE;
    raster.FrontCounterClockwise = FALSE;
    raster.DepthBias = 0;
    raster.DepthBiasClamp = 0.0f;
    raster.SlopeScaledDepthBias = 0.0f;
    raster.DepthClipEnable = TRUE;
    raster.MultisampleEnable = FALSE;
    raster.AntialiasedLineEnable = FALSE;
    raster.ForcedSampleCount = 0;
    raster.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return raster;
}

D3D12_DEPTH_STENCIL_DESC GraphicsPipelineInfo::no_depth() {
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depth.StencilEnable = FALSE;
    return depth;
}

bool create_graphics_pipeline(Device& device, ID3D12RootSignature* root, const GraphicsPipelineInfo& info,
                              Com<ID3D12PipelineState>* out, std::string* error) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.VS = info.vs;
    desc.PS = info.ps;
    desc.BlendState = info.blend;
    desc.SampleMask = 0xFFFFFFFFu;
    desc.RasterizerState = info.raster;
    if (info.samples > 1)
        desc.RasterizerState.MultisampleEnable = TRUE;
    desc.DepthStencilState = info.depth;
    desc.InputLayout.pInputElementDescs = info.inputs;
    desc.InputLayout.NumElements = info.input_count;
    desc.PrimitiveTopologyType = info.topology;
    if (info.color_format != DXGI_FORMAT_UNKNOWN) {
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = info.color_format;
    }
    desc.DSVFormat = info.depth_format;
    desc.SampleDesc.Count = info.samples;
    const HRESULT hr = device.d3d()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&*out));
    if (FAILED(hr))
        return fail(error, "CreateGraphicsPipelineState", hr);
    (*out)->SetName(info.name);
    return true;
}

}  // namespace shadow::render
