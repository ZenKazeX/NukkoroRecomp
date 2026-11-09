// Immediate geometry (docs/lift-20261006/r2/SITES_PIPELINES.md 2.8, 4.6): vertices the game hands over with each draw -
// text glyphs, trails, beams, the 2D, the screen effects' distortion meshes. The colour is the vertex colour, times
// the image when the draw has one (a texture, a snapshot of the scene, the view's history); no lights. Fog and the
// alpha cut-off as the meshes.
//
// Constant buffer b2: one 256-byte block per draw.

#include "glare_tiles.hlsli"

cbuffer Immediate : register(b2)
{
    float4x4 world_view_projection;
    uint texture;              // ResourceDescriptorHeap index, 0xFFFFFFFF = none
    uint sampler_index;
    uint fog_colour;           // RGBA8, R in the lowest byte
    uint fog_enabled;
    float cutoff;              // a texel at or below this alpha is not drawn (negative: everything drawn)
    float fog_start;
    float fog_end;
    uint flags;                // kSkipClear
    float4 uv_row[2];          // u' = r0 . (u, v, 1), v' = r1 . (u, v, 1): a screen image's canvas points placed
};

static const uint kSkipClear = 64;  // a fragment of alpha exactly 0 is not drawn (frame_math.h)

struct ImmediateVertex
{
    float3 position : POSITION;
    float4 colour : COLOR0;
    float2 uv : TEXCOORD0;
};

struct ImmediatePixel
{
    float4 position : SV_Position;
    float4 colour : COLOR0;
    float2 uv : TEXCOORD0;
    float depth : TEXCOORD1;   // eye depth, for fog
};

ImmediatePixel VSImmediate(ImmediateVertex v)
{
    ImmediatePixel o;
    // precise: no fused multiply-add the driver could choose differently in another compilation (mesh.hlsl).
    precise const float4 position = mul(world_view_projection, float4(v.position, 1.0));
    precise const float2 uv = float2(dot(uv_row[0].xyz, float3(v.uv, 1.0)), dot(uv_row[1].xyz, float3(v.uv, 1.0)));
    o.position = position;
    o.colour = v.colour;
    o.uv = uv;
    o.depth = position.w;
    return o;
}

GLARE_TILES_ENTRY float4 PSImmediate(ImmediatePixel i) : SV_Target
{
    precise float4 result = i.colour;
    if (texture != 0xFFFFFFFFu)
    {
        Texture2D<float4> image = ResourceDescriptorHeap[texture];
        SamplerState filter = SamplerDescriptorHeap[sampler_index];
        result *= image.Sample(filter, i.uv);
    }
    if (result.a <= cutoff)
        discard;
    if ((flags & kSkipClear) != 0 && result.a == 0.0)
        discard;
    if (fog_enabled != 0)
    {
        precise const float3 fog =
            float3(fog_colour & 0xFF, (fog_colour >> 8) & 0xFF, (fog_colour >> 16) & 0xFF) / 255.0;
        precise const float f = saturate((i.depth - fog_start) / max(fog_end - fog_start, 1e-6));
        result.rgb = lerp(result.rgb, fog, f);
    }
    mark_glare_tile(i.position);
    return result;
}
