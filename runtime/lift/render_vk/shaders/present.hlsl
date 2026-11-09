// The Vulkan renderer's own present-side shader (docs/VULKAN_RENDERER.md 3.2): the full-screen triangle of
// render/shaders/utility.hlsl VSFullscreen, with the picture turned by quarter turns - the surface pre-rotation of
// Android, where the swapchain image is in the display's native orientation and the compose draws the picture turned
// into it (instead of the compositor turning every frame). Paired with utility.hlsl's PSTexture (the same interface:
// SV_Position, TEXCOORD0 as uv). Nothing on D3D12 needs it: DXGI presents in the window's orientation.
//
// Root constants (b0) as utility.hlsl, plus:
//   root2.x  quarter turns clockwise the picture is drawn with (0..3; the surface's preTransform)

cbuffer Root : register(b0)
{
    uint4 root0;
    uint4 root1;
    uint4 root2;
    uint4 root3;
};

struct ScreenVertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// p: the position in the viewport (the turned rectangle), (0,0) its top-left; uv: the point of the picture shown there.
ScreenVertex VSFullscreenRotated(uint id : SV_VertexID)
{
    ScreenVertex o;
    const float2 p = float2((id << 1) & 2, id & 2);
    o.position = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    switch (root2.x & 3u) {
    case 1u: o.uv = float2(p.y, 1.0 - p.x); break;        // turned 90 degrees clockwise
    case 2u: o.uv = float2(1.0 - p.x, 1.0 - p.y); break;  // 180
    case 3u: o.uv = float2(1.0 - p.y, p.x); break;        // 270
    default: o.uv = p; break;
    }
    return o;
}
