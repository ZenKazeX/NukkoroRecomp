// Utility shaders of the renderer core (docs/lift-20261006/R_DESIGN.md 2.5-2.6): the full-screen triangle every
// screen pass draws, a triangle placed by root constants, a solid colour, a bindless texture read and the resolve of a
// multisampled target.
//
// Root constants (b0, 16 x 32-bit) as these shaders use them:
//   root0      colour (4 floats)
//   root1.x    texture: index into ResourceDescriptorHeap (PSResolve: the multisampled source)
//   root1.y    sampler: index into the sampler table (SamplerDescriptorHeap; PSResolve: the sample count)
//   root2/3.xy the triangle's three corners in normalised device coordinates (6 floats)

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

// One triangle that covers the whole target: uv (0,0) at the top-left corner, (1,1) at the bottom-right.
ScreenVertex VSFullscreen(uint id : SV_VertexID)
{
    ScreenVertex o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

ScreenVertex VSTriangle(uint id : SV_VertexID)
{
    const float2 corners[3] = {
        asfloat(root2.xy),
        asfloat(root2.zw),
        asfloat(root3.xy),
    };
    ScreenVertex o;
    o.position = float4(corners[min(id, 2u)], 0.0, 1.0);
    o.uv = o.position.xy * float2(0.5, -0.5) + 0.5;
    return o;
}

float4 PSSolid(ScreenVertex i) : SV_Target
{
    return asfloat(root0);
}

float4 PSTexture(ScreenVertex i) : SV_Target
{
    Texture2D<float4> source = ResourceDescriptorHeap[root1.x];
    SamplerState filter = SamplerDescriptorHeap[root1.y];
    return source.SampleLevel(filter, i.uv, 0.0);
}

// The present scale (G10, HW_REPORT N.2): the frame rendered at the render size (root1.x: the texture, root2.xy: its
// size in pixels) into the back buffer's viewport. root1.y = 1: an exact 2:1 reduction - the mean of the 2 x 2 source
// pixels under the output pixel (plain supersampling; exact in float, like PSResolve). root1.y = 0: any other ratio -
// a separable Catmull-Rom bicubic over the 4 x 4 source pixels around the output pixel's centre (clamped at the edges).
float4 PSScale(ScreenVertex i) : SV_Target
{
    Texture2D<float4> source = ResourceDescriptorHeap[root1.x];
    const int2 size = int2(root2.xy);
    if (root1.y == 1u) {
        const int2 p = int2(i.position.xy) * 2;
        precise float4 sum = source.Load(int3(p, 0)) + source.Load(int3(p + int2(1, 0), 0)) +
                             source.Load(int3(p + int2(0, 1), 0)) + source.Load(int3(p + int2(1, 1), 0));
        precise const float4 mean = sum * 0.25;
        return mean;
    }
    const float2 at = i.uv * float2(size) - 0.5;
    const float2 base = floor(at);
    const float2 f = at - base;
    float2 w[4];
    w[0] = f * (-0.5 + f * (1.0 - 0.5 * f));
    w[1] = 1.0 + f * f * (-2.5 + 1.5 * f);
    w[2] = f * (0.5 + f * (2.0 - 1.5 * f));
    w[3] = f * f * (-0.5 + 0.5 * f);
    float4 sum = 0;
    [unroll] for (int y = 0; y < 4; ++y) {
        [unroll] for (int x = 0; x < 4; ++x) {
            const int2 q = clamp(int2(base) + int2(x - 1, y - 1), int2(0, 0), size - 1);
            sum += source.Load(int3(q, 0)) * (w[x].x * w[y].y);
        }
    }
    return sum;
}

// Resolve in a fixed order. Precise prohibits reassociation and fusion so the
// sample mean is deterministic across shader compilations. UNORM loads are
// floats, not exact rational values; destination storage adds its own rounding.
float4 PSResolve(ScreenVertex i) : SV_Target
{
    Texture2DMS<float4> source = ResourceDescriptorHeap[root1.x];
    precise float4 sum = 0;
    for (uint s = 0; s < root1.y; ++s)
        sum += source.Load(int2(i.position.xy), s);
    precise const float4 mean = sum / float(root1.y);
    // Diagnostics (B3a): root3.x 1 returns the low byte of the 16-bit UNORM mean instead, 2..5 that of sample 0..3.
    if (root3.x == 1)
        return float4(fmod(round(saturate(mean.rgb) * 65535.0), 256.0) / 255.0, 1.0);
    if (root3.x >= 2)
        return float4(fmod(round(saturate(source.Load(int2(i.position.xy), root3.x - 2).rgb) * 65535.0), 256.0) / 255.0, 1.0);
    return mean;
}
