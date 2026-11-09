// Particles (docs/lift-20261006/r2/SITES_PIPELINES.md 2.7, 4.6): the game's particle systems as camera-facing quads,
// one instance per particle record, every particle of a batch in one draw.
//
// A particle at p with size (sx, sy) and rotation theta spans p +/- W +/- H with
//   W = sx/2 (cos theta R - sin theta U),  H = sy/2 (sin theta R + cos theta U)
// (R, U: the batch's axes - the camera's right and its up turned over, as the game's emitter takes them). Corners and
// texture coordinates as the emitter writes them: p - W - H (u1, v0), p + W - H (u0, v0), p + W + H (u0, v1),
// p - W + H (u1, v1); the quad's two triangles are (0, 1, 2) and (0, 2, 3). The console's emitter approximated sin
// and cos with a parabola (up to 5.6 % off); the intent is the rotation itself.
//
// Colour: the particle's own (per-particle colours) or the batch's; times the texture. Fog as the meshes: linear in
// eye depth from fog_start to fog_end, towards the fog colour.
//
// Constant buffer b2: one 256-byte block per batch.

#include "glare_tiles.hlsli"

cbuffer Particles : register(b2)
{
    float4x4 view_projection;
    float4 right;              // xyz: R
    float4 up;                 // xyz: U
    float4 colour;             // the batch colour (used without per-particle colours)
    uint texture;              // ResourceDescriptorHeap index, 0xFFFFFFFF = none
    uint sampler_index;
    uint flags;
    uint fog_colour;           // RGBA8, R in the lowest byte
    float cutoff;              // a texel at or below this alpha is not drawn (negative: everything drawn)
    float fog_start;
    float fog_end;
    uint fog_enabled;
};

static const uint kParticleColours = 1;  // each particle carries its colour
static const uint kSkipClear = 64;        // a fragment of alpha exactly 0 is not drawn (frame_math.h)

struct ParticleInstance
{
    float3 position : POSITION;
    float2 size : SIZE;
    float4 colour : COLOR0;
    float rotation : ROTATION;
    float2 uv0 : TEXCOORD0;    // top-left
    float2 uv1 : TEXCOORD1;    // bottom-right
};

struct ParticlePixel
{
    float4 position : SV_Position;
    float4 colour : COLOR0;
    float2 uv : TEXCOORD0;
    float depth : TEXCOORD1;   // eye depth, for fog
};

static const uint kTriangleCorners[6] = {0, 1, 2, 0, 2, 3};

ParticlePixel VSParticle(ParticleInstance p, uint vertex : SV_VertexID)
{
    const uint corner = kTriangleCorners[vertex];
    // precise: no fused multiply-add the driver could choose differently in another compilation (mesh.hlsl).
    precise float s, c;
    sincos(p.rotation, s, c);
    precise const float3 w = 0.5 * p.size.x * (c * right.xyz - s * up.xyz);
    precise const float3 h = 0.5 * p.size.y * (s * right.xyz + c * up.xyz);
    const float sign_w = corner == 1 || corner == 2 ? 1.0 : -1.0;
    const float sign_h = corner >= 2 ? 1.0 : -1.0;
    ParticlePixel o;
    precise const float4 position = mul(view_projection, float4(p.position + sign_w * w + sign_h * h, 1.0));
    o.position = position;
    o.colour = (flags & kParticleColours) != 0 ? p.colour : colour;
    o.uv = float2(corner == 0 || corner == 3 ? p.uv1.x : p.uv0.x, corner <= 1 ? p.uv0.y : p.uv1.y);
    o.depth = position.w;
    return o;
}

GLARE_TILES_ENTRY float4 PSParticle(ParticlePixel i) : SV_Target
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
