// The skin cache (docs/lift-20261006/R_DESIGN.md 2.4, 2.10): every skinned mesh is skinned once per frame, here,
// into the frame's skinned-vertex buffer; every pass that draws the mesh reads those vertices, so a second pass over
// the same mesh lands on exactly the same positions.
//
// A palette entry is a 3x4 matrix taking a bind-pose position to the pose this frame (column-vector convention:
// p' = P * [p, 1]); normals use its 3x3 part, as the game's own skinning does. Up to four weighted bones per vertex.
//
// Delta morphs (facial animation, the slingshot plant's band, flags, drapes) are blended here too, before skinning:
// the drawn shape is base + sum_t w_t delta_t with the draw's weights (the game's own blend, done in place on its
// arrays at every weight change, computes the same shape). The mesh's base and targets never change; each draw's shape
// lands in this frame's region like a skinned pose.
//
// Root constants (b0): root0.x source vertices (bindless structured buffer of SkinVertex), root0.y destination
// (bindless RW structured buffer of SkinnedVertex), root0.z vertex count, root0.w first destination element;
// root1.x delta targets (bindless structured buffer of MorphDelta, target t's vertex v at t * root0.z + v), root1.y
// target count (0: no morph), root1.z skinning (0: the blended vertices are written as they are, in model space);
// root2, root3: the weights of targets 0..7 (as float bits).
// Pass constants (b1): the palette.

cbuffer Root : register(b0)
{
    uint4 root0;
    uint4 root1;
    uint4 root2;
    uint4 root3;
};

cbuffer Palette : register(b1)
{
    row_major float3x4 bones[256];
};

struct SkinVertex
{
    float3 position;
    float3 normal;
    float4 weights;
    uint indices;  // four 8-bit bone indices, the first in the lowest byte
};

struct SkinnedVertex
{
    float3 position;
    float3 normal;
};

struct MorphDelta
{
    float3 position;
    float3 normal;
};

float morph_weight(uint t)
{
    return asfloat(t < 4 ? root2[t] : root3[t - 4]);
}

[numthreads(64, 1, 1)]
void CSSkin(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= root0.z)
        return;
    StructuredBuffer<SkinVertex> source = ResourceDescriptorHeap[root0.x];
    RWStructuredBuffer<SkinnedVertex> destination = ResourceDescriptorHeap[root0.y];
    const SkinVertex v = source[id.x];
    // precise: no fused multiply-add the driver could choose differently in another compilation (mesh.hlsl).
    precise float3 base_position = v.position;
    precise float3 base_normal = v.normal;
    if (root1.y != 0)
    {
        StructuredBuffer<MorphDelta> deltas = ResourceDescriptorHeap[root1.x];
        for (uint t = 0; t < root1.y; ++t)
        {
            const float w = morph_weight(t);
            if (w != 0)
            {
                const MorphDelta d = deltas[t * root0.z + id.x];
                base_position += w * d.position;
                base_normal += w * d.normal;
            }
        }
    }
    precise float3 position = 0;
    precise float3 normal = 0;
    if (root1.z == 0)
    {
        position = base_position;
        normal = base_normal;
    }
    else
    {
        [unroll]
        for (uint i = 0; i < 4; ++i)
        {
            const float w = v.weights[i];
            if (w != 0)
            {
                const float3x4 m = bones[(v.indices >> (8 * i)) & 0xFF];
                position += w * mul(m, float4(base_position, 1.0));
                normal += w * mul((float3x3)m, base_normal);
            }
        }
    }
    SkinnedVertex o;
    o.position = position;
    o.normal = normal;
    destination[root0.w + id.x] = o;
}
