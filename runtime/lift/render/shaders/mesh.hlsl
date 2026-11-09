// Mesh surfaces: the game's material program at full precision (docs/lift-20261006/R_DESIGN.md 2.3;
// r2/SITES_PIPELINES.md 4.1-4.5; R2_MATERIAL_CENSUS.md 6.2). One vertex function, three pixel functions:
//
//   PSBase  the base pass. Two combine forms, chosen per draw (flag kCombined):
//           one-stage   colour = C0 x (lit ? sat(base + D) : sat(base)) x texture,  base = P ? v : (X ? a k : 0)
//                       (the decision table's cases B, and the effects pipeline's base pass)
//           two-stage   colour = sat(C1 + C0 x R) [x texture],  R = (P ? v : 0), lit: sat(R + D);  C1 = C0 x a k
//                       (cases A and C)
//           C0 = M ? material colour : white; alpha = C0.a x (P ? v.a : 1) [x texture alpha].
//   PSEnvironment  the environment reflection: coefficient x environment texture, UV from the view-space normal
//                  (u = 1/2 + 1/2 n.x, v = 1/2 - 1/2 n.y with x screen right), added (blend ONE / ONE).
//   PSDual  the second texture over the base colour, from UV0 or UV1, blended by the material's pair.
//   PSBase with kBumpEnvironment: MatFX BUMPENVMAP, the original one-pass combine (four stages):
//           T = bumped texture at uv0 (height in its alpha), h0 = T.a, h1 = its alpha at uv0 + s (1 - 2 env),
//           C = sat(T x R); P = sat(sat(C x sat(1 - h0)) + h1 x C); colour = sat(P + k x E), alpha = R.a,
//           k = min(coefficient, 1) (x h1 with useFrameBufferAlpha), E = the environment map at the env UV.
//
// Every console clamp point is a saturate here; its byte truncations and quantised colours and normals are not.
// Fog closes every pass: linear in eye depth from fog_start to fog_end, towards the fog colour.
//
// Every output is `precise`: a driver may fuse a multiply and an add or not, differently from one compilation of the
// shader to another, and the last bit then shows in the picture (R2_DRIVER.md 4.9); precise forbids it.
//
// Constant buffers: b1 the draw's light set, b2 the draw (one 256-byte block per draw in the frame ring).

#include "glare_tiles.hlsli"

cbuffer Lights : register(b1)
{
    float4 ambient;            // rgb: the clamped sum of ambient lights
    uint4 light_counts;        // x directional lights, y point lights
    float4 directional_colour[4];
    float4 directional_vector[4];   // the direction the light travels, world space
    float4 point_position[8];       // xyz world position, w radius
    float4 point_colour[8];
};

cbuffer Draw : register(b2)
{
    float4x4 world_view_projection;
    float4 world_row[3];       // object to world (3x4)
    float4 view_row[3];        // object to view, 3x3 in .xyz (environment map normals)
    float4 colour;             // the material colour
    float4 uv_row[2];          // u' = dot(uv_row[0].xyz, (u, v, 1)), v' = dot(uv_row[1].xyz, (u, v, 1))
    uint texture;              // ResourceDescriptorHeap index, 0xFFFFFFFF = none
    uint sampler_index;
    uint flags;
    uint uv_set;               // dual pass: 0 or 1
    float cutoff;              // a texel at or below this alpha is not drawn (negative: everything drawn)
    float ka;                  // the material's ambient coefficient
    float coefficient;         // environment strength
    float fog_start;           // eye depth where fog begins
    float fog_end;             // eye depth where it is complete
    uint fog_colour;           // RGBA8, R in the lowest byte
    uint fog_enabled;
    float dissolve;            // the fade-in: share of pixels hidden (0 = all drawn)
};
// view_row[i].w (kBumpEnvironment only): [0] the bump scale s, [1] the environment map's descriptor, [2] its sampler.

static const uint kPrelit = 1;      // P: the vertex colours are the channel's source
static const uint kModulate = 2;    // M: the material colour enters
static const uint kLit = 4;         // L: the light set applies
static const uint kAmbient = 8;     // X: an ambient light is present
static const uint kCombined = 16;   // the two-stage form (cases A and C)
static const uint kUnbaked = 32;    // the baked light switched off: the vertex colours' RGB is white, alpha kept
static const uint kSkipClear = 64;  // a fragment of alpha exactly 0 is not drawn (frame_math.h)
static const uint kOpaqueCheck = 128;  // diagnostics: magenta where the exact opaque write's alpha is not 1
static const uint kOpaqueCheckVertex = 256;  // ... where the interpolated vertex alpha is not 1
static const uint kOpaqueCheckTexel = 512;   // ... where the filtered texel alpha is not 1
// Every vertex alpha (when P) and every texel alpha are 255 (proven from the asset data): the fragment alpha is
// exactly C0.a, taken from the constant instead of the interpolated / filtered value, whose arithmetic gives 1 - e for
// a constant 1 (docs/GPU_FILL.md section 19).
static const uint kExactAlpha = 1024;
static const uint kBumpEnvironment = 2048;       // MatFX BUMPENVMAP: the base pass is the bump + environment combine
static const uint kBumpFramebufferAlpha = 4096;  // ... the environment slot's useFrameBufferAlpha

struct MeshVertex
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 colour : COLOR0;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};

struct MeshPixel
{
    float4 position : SV_Position;
    float4 colour : COLOR0;         // the base pass's rasterised colour (before the texture)
    float2 uv0 : TEXCOORD0;         // transformed
    float2 uv1 : TEXCOORD1;
    float2 env : TEXCOORD2;         // environment-map coordinates
    float depth : TEXCOORD3;        // eye depth, for fog
};

float3 diffuse(float3 position, float3 normal)
{
    precise float3 sum = 0;
    for (uint i = 0; i < light_counts.x; ++i)
        sum += directional_colour[i].rgb * max(0.0, dot(normal, -directional_vector[i].xyz));
    for (uint j = 0; j < light_counts.y; ++j)
    {
        precise const float3 to_light = point_position[j].xyz - position;
        precise const float distance = length(to_light);
        const float radius = max(point_position[j].w, 1e-6);
        // The console's medium distance attenuation: brightness 1/2 at half the radius.
        precise const float attenuation =
            1.0 / (1.0 + distance / radius + 2.0 * distance * distance / (radius * radius));
        precise const float3 l = distance > 0 ? to_light / distance : float3(0, 0, 0);
        sum += point_colour[j].rgb * max(0.0, dot(normal, l)) * attenuation;
    }
    return sum;
}

MeshPixel VSMesh(MeshVertex v)
{
    MeshPixel o;
    precise const float4 position = mul(world_view_projection, float4(v.position, 1.0));
    o.position = position;
    precise const float3 world_position = float3(dot(world_row[0], float4(v.position, 1.0)),
                                                 dot(world_row[1], float4(v.position, 1.0)),
                                                 dot(world_row[2], float4(v.position, 1.0)));
    precise float3 world_normal = float3(dot(world_row[0].xyz, v.normal), dot(world_row[1].xyz, v.normal),
                                         dot(world_row[2].xyz, v.normal));
    precise const float normal_length = length(world_normal);
    world_normal = normal_length > 0 ? world_normal / normal_length : float3(0, 0, 0);

    const bool prelit = (flags & kPrelit) != 0;
    const bool modulate = (flags & kModulate) != 0;
    const bool lit = (flags & kLit) != 0;
    const float4 v_colour = prelit ? ((flags & kUnbaked) != 0 ? float4(1, 1, 1, v.colour.a) : v.colour)
                                   : float4(0, 0, 0, 1);
    precise const float3 ambient_term = (flags & kAmbient) != 0 ? ambient.rgb * ka : float3(0, 0, 0);
    precise const float3 d = lit ? diffuse(world_position, world_normal) : float3(0, 0, 0);
    const float4 c0 = modulate ? colour : float4(1, 1, 1, 1);
    precise float3 rgb;
    if ((flags & kCombined) != 0)
    {
        precise float3 r = prelit ? v_colour.rgb : float3(0, 0, 0);
        if (lit)
            r = saturate(r + d);
        rgb = saturate(c0.rgb * ambient_term + c0.rgb * r);
    }
    else
    {
        precise const float3 base = prelit ? v_colour.rgb : ambient_term;
        rgb = c0.rgb * (lit ? saturate(base + d) : saturate(base));
    }
    precise const float alpha = c0.a * (prelit ? v_colour.a : 1.0);
    o.colour = float4(rgb, alpha);
    precise const float2 uv0 = float2(dot(uv_row[0].xyz, float3(v.uv0, 1.0)), dot(uv_row[1].xyz, float3(v.uv0, 1.0)));
    precise const float2 uv1 = float2(dot(uv_row[0].xyz, float3(v.uv1, 1.0)), dot(uv_row[1].xyz, float3(v.uv1, 1.0)));
    o.uv0 = uv0;
    o.uv1 = uv_set != 0 ? uv1 : uv0;
    precise float3 view_normal = float3(dot(view_row[0].xyz, v.normal), dot(view_row[1].xyz, v.normal),
                                        dot(view_row[2].xyz, v.normal));
    precise const float view_length = length(view_normal);
    view_normal = view_length > 0 ? view_normal / view_length : float3(0, 0, 0);
    precise const float2 env = float2(0.5 + 0.5 * view_normal.x, 0.5 - 0.5 * view_normal.y);
    o.env = env;
    o.depth = position.w;
    return o;
}

float4 sample_texture(float2 uv)
{
    Texture2D<float4> image = ResourceDescriptorHeap[texture];
    SamplerState filter = SamplerDescriptorHeap[sampler_index];
    return image.Sample(filter, uv);
}

// The fog blend, the last step of every pass (so an additive pass adds the fog colour's share too).
float3 fogged(float3 rgb, float depth)
{
    if (fog_enabled == 0)
        return rgb;
    precise const float3 fog = float3(fog_colour & 0xFF, (fog_colour >> 8) & 0xFF, (fog_colour >> 16) & 0xFF) / 255.0;
    precise const float f = saturate((depth - fog_start) / max(fog_end - fog_start, 1e-6));
    precise const float3 result = lerp(rgb, fog, f);
    return result;
}

// The fade-in (gekko/appear_fade.h): a 4x4 ordered (Bayer) threshold per pixel; a draw hides the pixels whose threshold is below its
// dissolve, so the hidden share falls evenly to 0 as the object fades in. Opaque surfaces stay opaque and keep their
// depth: no blending, no sorting.
void dissolve_test(float4 position)
{
    if (dissolve <= 0.0)
        return;
    static const float kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    const uint2 p = uint2(position.xy) & 3u;
    if ((kBayer[p.y * 4u + p.x] + 0.5) / 16.0 < dissolve)
        discard;
}

// MatFX BUMPENVMAP. The original combine, stage by stage:
//   0  map0 (the bumped texture) at uv0, ras COLOR0A0:  C0 = sat(TEXC x RASC);      A = sat(KONST 1 x (1 - TEXA))
//   1  map0 at uv0 + coord1 (indirect add-previous, no matrix):  C = sat(APREV x C0);  A = TEXA
//   2  no texture:  C = sat(CPREV + APREV x C0);  A = KONST (K0.a = coefficient), x APREV with useFrameBufferAlpha
//   3  map1 (the environment map) at coord2, ras COLOR0A0:  C = sat(CPREV + APREV x TEXC);  A = RASA
// coord1 = TEXMTX0 x normal: s x rows 0, 1 of the env matrix, no translation; coord2 = the environment UV (the same
// -0.5 / +0.5 matrix as PSEnvironment). In the screen-right view frame both are i.env-derived: (M n).xy = 1 - 2 env.
float4 bump_environment(MeshPixel i)
{
    const float4 texel = texture != 0xFFFFFFFFu ? sample_texture(i.uv0) : float4(1, 1, 1, 1);
    precise const float2 shifted_uv = i.uv0 + view_row[0].w * (1.0 - 2.0 * i.env);
    const float h1 = texture != 0xFFFFFFFFu ? sample_texture(shifted_uv).a : 1.0;
    Texture2D<float4> environment_map = ResourceDescriptorHeap[asuint(view_row[1].w)];
    SamplerState environment_filter = SamplerDescriptorHeap[asuint(view_row[2].w)];
    const float4 environment = environment_map.Sample(environment_filter, i.env);
    precise const float3 c0 = saturate(texel.rgb * i.colour.rgb);
    precise const float3 stage1 = saturate(c0 * saturate(1.0 - texel.a));
    precise const float3 stage2 = saturate(stage1 + h1 * c0);
    precise const float k = min(coefficient, 1.0) * ((flags & kBumpFramebufferAlpha) != 0 ? h1 : 1.0);
    precise const float3 stage3 = saturate(stage2 + k * environment.rgb);
    return float4(stage3, i.colour.a);
}

GLARE_TILES_ENTRY float4 PSBase(MeshPixel i) : SV_Target
{
    dissolve_test(i.position);
    if ((flags & kBumpEnvironment) != 0)
    {
        precise float4 bumped = bump_environment(i);
        if ((flags & kExactAlpha) != 0)
            bumped.a = (flags & kModulate) != 0 ? colour.a : 1.0;
        if ((flags & kSkipClear) != 0 && bumped.a == 0.0)
            discard;
        precise const float4 bumped_out = float4(fogged(bumped.rgb, i.depth), bumped.a);
        mark_glare_tile(i.position);
        return bumped_out;
    }
    precise float4 result = i.colour;
    float texel_alpha = 1.0;
    if (texture != 0xFFFFFFFFu)
    {
        const float4 texel = sample_texture(i.uv0);
        texel_alpha = texel.a;
        result *= texel;
    }
    if ((flags & kOpaqueCheckVertex) != 0 && i.colour.a != 1.0)
        return float4(1, 0, 1, 1);
    if ((flags & kOpaqueCheckTexel) != 0 && texel_alpha != 1.0)
        return float4(1, 0, 1, 1);
    if ((flags & kExactAlpha) != 0)
        result.a = (flags & kModulate) != 0 ? colour.a : 1.0;
    if (result.a <= cutoff)
        discard;
    if ((flags & kSkipClear) != 0 && result.a == 0.0)
        discard;
    if ((flags & kOpaqueCheck) != 0 && result.a != 1.0)
        return float4(1, 0, 1, 1);
    precise const float4 out_colour = float4(fogged(result.rgb, i.depth), result.a);
    mark_glare_tile(i.position);
    return out_colour;
}

GLARE_TILES_ENTRY float4 PSEnvironment(MeshPixel i) : SV_Target
{
    dissolve_test(i.position);
    const float4 environment = sample_texture(i.env);
    precise const float4 out_colour = float4(fogged(min(coefficient, 1.0) * environment.rgb, i.depth), environment.a);
    mark_glare_tile(i.position);
    return out_colour;
}

GLARE_TILES_ENTRY float4 PSDual(MeshPixel i) : SV_Target
{
    dissolve_test(i.position);
    precise float4 result = i.colour * sample_texture(i.uv1);
    if (result.a <= cutoff)
        discard;
    if ((flags & kSkipClear) != 0 && result.a == 0.0)
        discard;
    precise const float4 out_colour = float4(fogged(result.rgb, i.depth), result.a);
    mark_glare_tile(i.position);
    return out_colour;
}
