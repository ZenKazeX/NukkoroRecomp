// Native renderer shaders (docs/native-runtime-2026-09-16/ARCHITECTURE.md §3).
//
// Compiled at build time with DXC (SM 6.0) and embedded. These are not a TEV
// interpreter: the combiner is one of the closed set of RwGC material programs
// (MaterialProgram in gekko/native_frame.h), selected by the capture from
// RenderWare semantics. The fixed-function parts - GX channel lighting, texture
// coordinate generation, alpha compare and fog - implement the hardware
// equations from the state values the driver set.
//
// Entry points:
//   VSMain            all draws
//   PSMain            no alpha test (early depth stays enabled)
//   PSMainAlphaTest   alpha compare with discard (late depth, as GXSetZCompLoc(FALSE))
//   PSMainFootprint / PSMainAlphaTestFootprint
//                     the same two with guest-texel footprint reconstruction on
//                     map 0, for draws that sample a resident EFB copy without
//                     magnifying it (the glare ladder). Separate entry points so
//                     the ordinary path stays exactly what it was.

// ---- resources ----------------------------------------------------------------

Texture2D<float4> g_textures[] : register(t0, space0);
SamplerState g_samplers[36] : register(s0, space1);

cbuffer DrawConstants : register(b0, space2) {
    float4 pos_row0;
    float4 pos_row1;
    float4 pos_row2;
    float4 nrm_row0;
    float4 nrm_row1;
    float4 nrm_row2;
    float4 proj_a;        // x: type (0 persp, 1 ortho), y..w: p0 p1 p2
    float4 proj_b;        // x..z: p3 p4 p5
    float4 tex0_row0;
    float4 tex0_row1;
    float4 tex0_row2;
    float4 tex1_row0;
    float4 tex1_row1;
    float4 tex1_row2;
    uint texgen0;         // func | src << 8 | enabled << 16
    uint texgen1;
    uint channel_count;
    uint vertex_flags;    // bit0 has normal, bit1 has color0, bit2 has color1
    uint chan0_color;     // enable | amb << 1 | mat << 2 | diffuse << 4 | attn << 6 | mask << 8
    uint chan0_alpha;
    uint chan1_color;
    uint chan1_alpha;
    float4 mat_color0;
    float4 amb_color0;
    float4 mat_color1;
    float4 amb_color1;
    float4 tev_reg0;
    float4 tev_reg1;
    float4 fog_color;
    float4 fog_params;    // A, B, C, (unused)
    uint program;         // MaterialProgram | modulate operand order << 8 (program_id())
    uint alpha_compare;   // comp0 | ref0 << 3 | op << 11 | comp1 << 13 | ref1 << 16
    uint texture0;
    uint texture1;
    uint sampler0;
    uint sampler1;
    uint fog_type;        // GXFogType
    uint blend_premultiply; // 0 none, 1 rgb*a (SRCALPHA), 2 rgb*(1-a) (INVSRCALPHA), truncated
    float4 uv_transform0; // xy scale, zw offset (camera-target sub rectangles)
    float4 uv_transform1;
    float4 hud_affine;    // canvas x' = ax*x + bx, y' = ay*y + by (widescreen 2D layout)
    float4 viewport_rect; // guest viewport x, y, width, height (for the canvas mapping)
    uint hud_enabled;
    // Guest-texel footprint for map 0 when the footprint entry points run:
    // taps_x | taps_y << 8, the native texels per guest texel of the bound
    // resident copy. 0 or 1 in both lanes means no reconstruction.
    uint footprint_taps0;
    uint pad2;
    uint pad3;
};

cbuffer LightConstants : register(b1, space2) {
    float4 light_color[8];
    float4 light_position[8];
    float4 light_direction[8];
    float4 light_cos_attenuation[8];
    float4 light_dist_attenuation[8];
};

// ---- vertex stage ---------------------------------------------------------------

struct VertexInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 color0 : COLOR0;
    float4 color1 : COLOR1;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};

struct PixelInput {
    float4 position : SV_Position;
    float4 color0 : COLOR0;
    float4 color1 : COLOR1;
    float3 uv0 : TEXCOORD0;   // z = q (3x4 texgen)
    float3 uv1 : TEXCOORD1;
};

static const uint kProgramPassColor = 0;
static const uint kProgramModulate = 1;
static const uint kProgramDefaultTx2 = 2;
static const uint kProgramDefaultUt1 = 3;
static const uint kProgramUnsupported = 4;

float3 transform_row3(float4 r0, float4 r1, float4 r2, float4 v) {
    return float3(dot(r0, v), dot(r1, v), dot(r2, v));
}

// GX lighting for one channel component set. `material` and `ambient` are the
// already-selected sources; returns the lit value (unclamped result is clamped
// per the hardware). The light loop follows the XF equations: SPOT attenuation
// uses the light direction cosine and the distance polynomial; NONE uses no
// attenuation; the diffuse function is NONE (1), SIGN (N.L) or CLAMP (max 0).
float4 light_channel(uint control, float4 material, float4 ambient, float3 eye_position,
                     float3 eye_normal, bool alpha) {
    uint enable = control & 1u;
    if (enable == 0u) return material;
    uint diffuse_fn = (control >> 4) & 3u;
    uint attenuation_fn = (control >> 6) & 3u;
    uint mask = (control >> 8) & 255u;
    float4 accumulated = saturate(ambient);
    for (uint index = 0u; index < 8u; ++index) {
        if (((mask >> index) & 1u) == 0u) continue;
        float3 to_light = light_position[index].xyz - eye_position;
        float distance2 = dot(to_light, to_light);
        float distance = sqrt(distance2);
        float3 direction = distance > 0.0 ? to_light / distance : float3(0.0, 0.0, 0.0);
        float attenuation = 1.0;
        // GXAttnFn: GX_AF_SPEC 0, GX_AF_SPOT 1, GX_AF_NONE 2.
        if (attenuation_fn == 1u) {        // GX_AF_SPOT
            float cosine = max(0.0, dot(direction, light_direction[index].xyz));
            float angular = max(0.0, light_cos_attenuation[index].x +
                                          light_cos_attenuation[index].y * cosine +
                                          light_cos_attenuation[index].z * cosine * cosine);
            float radial = light_dist_attenuation[index].x +
                           light_dist_attenuation[index].y * distance +
                           light_dist_attenuation[index].z * distance2;
            attenuation = radial != 0.0 ? angular / radial : 0.0;
        } else if (attenuation_fn == 0u) { // GX_AF_SPEC
            float3 half_angle = normalize(light_direction[index].xyz);
            float cosine = eye_normal.x == 0.0 && eye_normal.y == 0.0 && eye_normal.z == 0.0
                               ? 0.0
                               : max(0.0, dot(eye_normal, half_angle));
            float angular = max(0.0, light_cos_attenuation[index].x +
                                          light_cos_attenuation[index].y * cosine +
                                          light_cos_attenuation[index].z * cosine * cosine);
            float radial = light_dist_attenuation[index].x +
                           light_dist_attenuation[index].y * cosine +
                           light_dist_attenuation[index].z * cosine * cosine;
            attenuation = radial != 0.0 ? angular / radial : 0.0;
        }
        float diffuse = 1.0;
        if (diffuse_fn == 1u) diffuse = dot(eye_normal, direction);
        else if (diffuse_fn == 2u) diffuse = max(0.0, dot(eye_normal, direction));
        accumulated += light_color[index] * (attenuation * diffuse);
    }
    return material * saturate(accumulated);
}

float3 texgen(uint config, float4 r0, float4 r1, float4 r2, float4 position, float3 normal,
              float2 uv0, float2 uv1) {
    uint function = config & 255u;
    uint source = (config >> 8) & 255u;
    float4 input = float4(0.0, 0.0, 1.0, 1.0);
    if (source == 0u) input = position;                    // GX_TG_POS
    else if (source == 1u) input = float4(normal, 1.0);   // GX_TG_NRM (model space)
    else if (source == 4u) input = float4(uv0, 1.0, 1.0); // GX_TG_TEX0
    else if (source == 5u) input = float4(uv1, 1.0, 1.0); // GX_TG_TEX1
    if (function == 1u) {                                  // GX_TG_MTX2x4
        return float3(dot(r0, input), dot(r1, input), 1.0);
    }
    if (function == 0u) {                                  // GX_TG_MTX3x4
        float3 st = float3(dot(r0, input), dot(r1, input), dot(r2, input));
        return st;
    }
    return float3(input.xy, 1.0);
}

PixelInput VSMain(VertexInput input) {
    PixelInput output;
    float4 model = float4(input.position, 1.0);
    float3 eye = transform_row3(pos_row0, pos_row1, pos_row2, model);
    float4 clip;
    if (proj_a.x < 0.5) {
        clip = float4(proj_a.y * eye.x + proj_a.z * eye.z, proj_a.w * eye.y + proj_b.x * eye.z,
                      proj_b.y * eye.z + proj_b.z, -eye.z);
    } else {
        clip = float4(proj_a.y * eye.x + proj_a.z, proj_a.w * eye.y + proj_b.x,
                      proj_b.y * eye.z + proj_b.z, 1.0);
    }
    // GX window depth is 1 + z/w; D3D takes z/w directly.
    clip.z = clip.z + clip.w;
    if (hud_enabled != 0u) {
        // Widescreen 2D layout: remap the draw in guest canvas (window) space.
        float w = clip.w != 0.0 ? clip.w : 1.0;
        float window_x = viewport_rect.x + (clip.x / w * 0.5 + 0.5) * viewport_rect.z;
        float window_y = viewport_rect.y + (0.5 - clip.y / w * 0.5) * viewport_rect.w;
        float mapped_x = hud_affine.x * window_x + hud_affine.y;
        float mapped_y = hud_affine.z * window_y + hud_affine.w;
        clip.x = (((mapped_x - viewport_rect.x) / viewport_rect.z) * 2.0 - 1.0) * w;
        clip.y = (0.5 - (mapped_y - viewport_rect.y) / viewport_rect.w) * 2.0 * w;
    }
    output.position = clip;

    float3 eye_normal = float3(0.0, 0.0, 0.0);
    if ((vertex_flags & 1u) != 0u) {
        float3 n = float3(dot(nrm_row0.xyz, input.normal), dot(nrm_row1.xyz, input.normal),
                          dot(nrm_row2.xyz, input.normal));
        float len = length(n);
        eye_normal = len > 0.0 ? n / len : n;
    }
    float4 vertex0 = (vertex_flags & 2u) != 0u ? input.color0 : float4(1.0, 1.0, 1.0, 1.0);
    float4 vertex1 = (vertex_flags & 4u) != 0u ? input.color1 : float4(1.0, 1.0, 1.0, 1.0);

    float4 color0 = float4(0.0, 0.0, 0.0, 0.0);
    float4 color1 = float4(0.0, 0.0, 0.0, 0.0);
    if (channel_count >= 1u) {
        float4 material_rgb = ((chan0_color >> 2) & 1u) != 0u ? vertex0 : mat_color0;
        float4 ambient_rgb = ((chan0_color >> 1) & 1u) != 0u ? vertex0 : amb_color0;
        float4 material_a = ((chan0_alpha >> 2) & 1u) != 0u ? vertex0 : mat_color0;
        float4 ambient_a = ((chan0_alpha >> 1) & 1u) != 0u ? vertex0 : amb_color0;
        color0.rgb = light_channel(chan0_color, material_rgb, ambient_rgb, eye, eye_normal, false).rgb;
        color0.a = light_channel(chan0_alpha, material_a, ambient_a, eye, eye_normal, true).a;
    } else {
        color0 = vertex0;
    }
    if (channel_count >= 2u) {
        float4 material_rgb = ((chan1_color >> 2) & 1u) != 0u ? vertex1 : mat_color1;
        float4 ambient_rgb = ((chan1_color >> 1) & 1u) != 0u ? vertex1 : amb_color1;
        float4 material_a = ((chan1_alpha >> 2) & 1u) != 0u ? vertex1 : mat_color1;
        float4 ambient_a = ((chan1_alpha >> 1) & 1u) != 0u ? vertex1 : amb_color1;
        color1.rgb = light_channel(chan1_color, material_rgb, ambient_rgb, eye, eye_normal, false).rgb;
        color1.a = light_channel(chan1_alpha, material_a, ambient_a, eye, eye_normal, true).a;
    }
    output.color0 = saturate(color0);
    output.color1 = saturate(color1);

    output.uv0 = float3(0.0, 0.0, 1.0);
    output.uv1 = float3(0.0, 0.0, 1.0);
    if (((texgen0 >> 16) & 1u) != 0u) {
        output.uv0 = texgen(texgen0, tex0_row0, tex0_row1, tex0_row2, model, input.normal,
                            input.uv0, input.uv1);
    }
    if (((texgen1 >> 16) & 1u) != 0u) {
        output.uv1 = texgen(texgen1, tex1_row0, tex1_row1, tex1_row2, model, input.normal,
                            input.uv0, input.uv1);
    }
    return output;
}

// ---- pixel stage ---------------------------------------------------------------

float4 sample_map(uint texture_index, uint sampler_index, float3 uvq, float4 transform) {
    float2 uv = uvq.z != 0.0 ? uvq.xy / uvq.z : uvq.xy;
    uv = uv * transform.xy + transform.zw;
    return g_textures[NonUniformResourceIndex(texture_index)].Sample(
        g_samplers[NonUniformResourceIndex(sampler_index)], uv);
}

// Guest-texel footprint reconstruction for a resident EFB copy.
//
// The glare ladder's taps sit ONE GUEST TEXEL apart (its +/-1 and +/-2 blur
// taps, its 5:1 downsample). On a copy that holds `taps` native texels per
// guest texel, those taps read single native sub-texels `taps` apart: a sparse
// comb with unit gain at the guest-texel period, so native-frequency grain
// survives every blur pass and the bloom carries a lattice of exactly one
// ladder texel. Averaging the taps_x x taps_y native texels CENTRED on the
// sample turns them back into the contiguous kernel the guest authored, at
// native output density.
//
// This is the same reconstruction the legacy GX raster already uses
// (MotionHistoryReconstruction::EffectFootprint, gx_raster_shader.h
// SampleTextureFootprint). It lives in its own entry points, not in PSMain:
// the common path must stay byte-identical, and at scale 1 taps are 1x1 so the
// whole thing collapses to the ordinary sample by construction.
float4 sample_map_footprint(uint texture_index, uint sampler_index, float3 uvq,
                            float4 transform, uint taps_packed) {
    uint taps_x = max(taps_packed & 255u, 1u);
    uint taps_y = max((taps_packed >> 8) & 255u, 1u);
    if (taps_x <= 1u && taps_y <= 1u) {
        return sample_map(texture_index, sampler_index, uvq, transform);
    }
    float2 uv = uvq.z != 0.0 ? uvq.xy / uvq.z : uvq.xy;
    uv = uv * transform.xy + transform.zw;
    uint width = 0, height = 0;
    g_textures[NonUniformResourceIndex(texture_index)].GetDimensions(width, height);
    if (width == 0u || height == 0u) {
        return sample_map(texture_index, sampler_index, uvq, transform);
    }
    // Half a native texel to the lower right for even tap counts; exactly one
    // guest texel for odd ones - the same centring the legacy path uses.
    float2 texel = float2(1.0 / float(width), 1.0 / float(height));
    float2 origin = uv - float2(float(taps_x) * 0.5 - 0.5, float(taps_y) * 0.5 - 0.5) * texel;
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    [loop] for (uint oy = 0u; oy < taps_y; ++oy) {
        [loop] for (uint ox = 0u; ox < taps_x; ++ox) {
            float2 at = origin + float2(float(ox), float(oy)) * texel;
            sum += g_textures[NonUniformResourceIndex(texture_index)].Sample(
                g_samplers[NonUniformResourceIndex(sampler_index)], at);
        }
    }
    return sum / float(taps_x * taps_y);
}

// TEV works on 8-bit integers. A regular stage with operands a, b, c, d and
// op add, bias zero, scale one computes
//     c' = c + (c >> 7)
//     d + ((a * (256 - c') + b * c' + 128) >> 8)
// clamped to 0..255 - the Flipper's 8.8 lerp (Dolphin Tev.cpp DrawColorRegular
// and PixelShaderGen, identical). It is NOT symmetric in b and c: the operand
// in the c slot gets the c >> 7 correction, so every program carries which
// input sits where. Integer arithmetic here - instead of float products
// written through UNORM rounding - is also what keeps additive chains such as
// the glare ladder from drifting one rounding step per pass.
int4 to_byte(float4 value) {
    return int4(floor(saturate(value) * 255.0 + 0.5));
}

int4 tev_lerp(int4 a, int4 b, int4 c, int4 d) {
    int4 weight = c + (c >> 7);
    return clamp(d + ((a * (256 - weight) + b * weight + 128) >> 8), 0, 255);
}

// DrawConstants.program: the MaterialProgram in bits 0-7, and in bit 8 the
// operand order of a MODULATE stage. 0 = the GXSetTevOp(GX_MODULATE) preset
// (b = texture, c = rasterised colour: Im2D, Im3D, PTank, MatFX, disc error);
// 1 = the RwGC one-stage table at 0x804669A8 (b = rasterised, c = texture:
// the textured default object and skin).
uint program_id() {
    return program & 255u;
}

bool modulate_table_order() {
    return ((program >> 8) & 1u) != 0u;
}

// footprint is a compile-time constant per entry point: PSMain/PSMainAlphaTest
// pass false, so the reconstruction below is dead code there and the ordinary
// path is exactly what it was.
float4 sample_map0(PixelInput input, bool footprint) {
    if (footprint) {
        return sample_map_footprint(texture0, sampler0, input.uv0, uv_transform0,
                                    footprint_taps0);
    }
    return sample_map(texture0, sampler0, input.uv0, uv_transform0);
}

float4 combine(PixelInput input, bool footprint) {
    int4 ras = to_byte(input.color0);
    int4 zero = int4(0, 0, 0, 0);
    uint id = program_id();
    int4 result;
    if (id == kProgramPassColor) {
        result = ras;
    } else if (id == kProgramModulate) {
        int4 tex = to_byte(sample_map0(input, footprint));
        result = modulate_table_order() ? tev_lerp(zero, ras, tex, zero)
                                        : tev_lerp(zero, tex, ras, zero);
    } else if (id == kProgramDefaultTx2 || id == kProgramDefaultUt1) {
        // Stage 0: a = ZERO, b = RAS, c = REG0, d = REG1.
        int4 reg0 = int4(tev_reg0 * 255.0 + 0.5);
        int4 reg1 = int4(tev_reg1 * 255.0 + 0.5);
        int4 stage0 = tev_lerp(zero, ras, reg0, reg1);
        if (id == kProgramDefaultUt1) {
            result = stage0;
        } else {
            // Stage 1: a = ZERO, b = PREV, c = TEX, d = ZERO.
            int4 tex = to_byte(sample_map0(input, footprint));
            result = tev_lerp(zero, stage0, tex, zero);
        }
    } else {
        return float4(1.0, 0.0, 1.0, 1.0);
    }
    return float4(result) / 255.0;
}

bool compare_alpha(uint function, uint alpha, uint reference) {
    switch (function) {
        case 0u: return false;                 // NEVER
        case 1u: return alpha < reference;     // LESS
        case 2u: return alpha == reference;    // EQUAL
        case 3u: return alpha <= reference;    // LEQUAL
        case 4u: return alpha > reference;     // GREATER
        case 5u: return alpha != reference;    // NEQUAL
        case 6u: return alpha >= reference;    // GEQUAL
        default: return true;                  // ALWAYS
    }
}

float4 apply_fog(float4 color, float depth) {
    uint type = fog_type & 7u;
    if (type == 0u) return color;
    float z;
    if ((fog_type & 8u) != 0u) {
        z = fog_params.x * depth - fog_params.z;           // orthographic
    } else {
        float denominator = fog_params.y - depth;
        z = denominator != 0.0 ? fog_params.x / denominator - fog_params.z : 1.0;
    }
    float fog = saturate(z);
    if (type == 4u) fog = 1.0 - pow(2.0, -8.0 * fog);
    else if (type == 5u) fog = 1.0 - pow(2.0, -8.0 * fog * fog);
    else if (type == 6u) fog = pow(2.0, -8.0 * (1.0 - fog));
    else if (type == 7u) { float r = 1.0 - fog; fog = pow(2.0, -8.0 * r * r); }
    // The fog blend is 8-bit integer arithmetic with an 8.8 weight, like the
    // rest of this shader's TEV output: (c*(256-f) + fogc*f) >> 8, truncating.
    // The weight is rounded (Dolphin's hardware backend, PixelShaderGen
    // `iround(fog * 256.0)`; its software backend truncates the weight - an
    // open oracle question). A float lerp rounded by the UNORM target was
    // neither.
    int3 value = to_byte(color).rgb;
    int3 fogc = to_byte(fog_color).rgb;
    int weight = (int)round(fog * 256.0);
    value = (value * (256 - weight) + fogc * weight) >> 8;
    return float4(float3(value) / 255.0, color.a);
}

// GX blends each term with 8-bit truncation: src*factor/255. When the source
// factor is (INV)SRCALPHA the pipeline blends ONE instead and the product is
// taken here with the same truncation, so additive effects accumulate exactly.
float4 finish(float4 color, float depth) {
    color = apply_fog(color, depth);
    if (blend_premultiply != 0u) {
        int4 value = to_byte(color);
        int factor = blend_premultiply == 1u ? value.a : 255 - value.a;
        value.rgb = (value.rgb * factor) / 255;
        color = float4(value) / 255.0;
    }
    return color;
}

float4 PSMain(PixelInput input) : SV_Target0 {
    float4 color = combine(input, false);
    return finish(color, input.position.z);
}

// Same shader with the guest-texel footprint reconstruction enabled for map 0.
// Selected by the pipeline key only for draws that sample a resident EFB copy
// without magnifying it - the glare ladder's blur taps and its downsample.
float4 PSMainFootprint(PixelInput input) : SV_Target0 {
    float4 color = combine(input, true);
    return finish(color, input.position.z);
}

float4 PSMainAlphaTest(PixelInput input) : SV_Target0 {
    float4 color = combine(input, false);
    uint alpha = (uint)floor(saturate(color.a) * 255.0 + 0.5);
    uint comp0 = alpha_compare & 7u;
    uint ref0 = (alpha_compare >> 3) & 255u;
    uint op = (alpha_compare >> 11) & 3u;
    uint comp1 = (alpha_compare >> 13) & 7u;
    uint ref1 = (alpha_compare >> 16) & 255u;
    bool a = compare_alpha(comp0, alpha, ref0);
    bool b = compare_alpha(comp1, alpha, ref1);
    bool pass;
    if (op == 0u) pass = a && b;           // AND
    else if (op == 1u) pass = a || b;      // OR
    else if (op == 2u) pass = a != b;      // XOR
    else pass = a == b;                    // XNOR
    if (!pass) discard;
    return finish(color, input.position.z);
}

// The alpha-tested twin of PSMainFootprint. The ladder's own passes are not
// alpha tested today, but the pipeline key can pair footprint with alpha test
// and a missing variant would silently fall back to point sampling.
float4 PSMainAlphaTestFootprint(PixelInput input) : SV_Target0 {
    float4 color = combine(input, true);
    uint alpha = (uint)floor(saturate(color.a) * 255.0 + 0.5);
    uint comp0 = alpha_compare & 7u;
    uint ref0 = (alpha_compare >> 3) & 255u;
    uint op = (alpha_compare >> 11) & 3u;
    uint comp1 = (alpha_compare >> 13) & 7u;
    uint ref1 = (alpha_compare >> 16) & 255u;
    bool a = compare_alpha(comp0, alpha, ref0);
    bool b = compare_alpha(comp1, alpha, ref1);
    bool pass;
    if (op == 0u) pass = a && b;
    else if (op == 1u) pass = a || b;
    else if (op == 2u) pass = a != b;
    else pass = a == b;
    if (!pass) discard;
    return finish(color, input.position.z);
}
