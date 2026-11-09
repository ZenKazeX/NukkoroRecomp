#pragma once

// The presenter's own blit program, the only shader the shipping executable
// embeds. Baked to DXBC by tools/shaderbake (set "present"); the runtime does
// not compile it.

// The presentation blit is a SEPARATE shader source on purpose. plume derives
// the HLSL register space from the descriptor-set index
// (plume_d3d12.cpp: `staticSampler.RegisterSpace = i`), so a layout with one
// set must declare its resources in space0 - and space0 is already taken in the
// raster program by the unbounded `gxTextures[]` array. Compiling this
// independently lets it use space0 without colliding. Getting this wrong bound
// nothing at all: the shader sampled a null descriptor and the adapter TDR'd.
constexpr const char* kGxPresentShader = R"hlsl(
Texture2D<float4> gxPresentSource : register(t0, space0);
Texture2D<float4> gxPresentPrevious : register(t1, space0);
SamplerState gxPresentSampler : register(s0, space0);
cbuffer GxPresentBlend : register(b0, space0)
{
    float gxPresentAlpha;
    // Upper bound on area-resolve taps per axis. 1 reproduces the original
    // single bilinear tap exactly, which is the A/B arm and the 1x behaviour.
    int gxPresentMaxTaps;
};

struct GxPresentVSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

GxPresentVSOutput VSPresentBlit(uint gxVertexId : SV_VertexID)
{
    // Single oversized triangle rather than a quad: no diagonal seam and one
    // fewer primitive. Vertices 0,1,2 give uv (0,0), (2,0), (0,2).
    float2 gxUv = float2((gxVertexId << 1u) & 2u, gxVertexId & 2u);
    GxPresentVSOutput gxOut;
    gxOut.uv = gxUv;
    gxOut.position = float4(gxUv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return gxOut;
}

// ---------------------------------------------------------------------------
// Area resolve
// ---------------------------------------------------------------------------
//
// This is the ONLY path the window goes through, and it is the one no gate
// measures: `--screenshot` writes the guest XFB through an exact integer box
// resolve, so it cannot see what happens here. Measured on a Final Haunt burst
// frame at internal scale 3, isolated single-pixel outliers were 3 on the
// screenshot and 813 on the presented 1440x1080 image.
//
// The previous heuristic took `ceil(ratio / 2)` uniformly weighted bilinear
// taps, on the reasoning that one bilinear tap covers two source texels. Two
// things were wrong with it. At internal scale 3 into a 1080p window the
// minification ratio is 1.333, which returns ONE tap - a single centred
// bilinear sample that under-weights the edges of the footprint. And raising
// the count does not fix it: N uniformly weighted taps spread across the
// footprint is a WIDER filter than the footprint, so it trades speckle for
// over-blur (measured: 2x2 cut outliers 813 -> 249 but its error against an
// exact area resolve rose, 0.2886 -> 0.2899).
//
// What follows is the exact box instead of an approximation of one. A
// destination pixel covers the source range [centre - f/2, centre + f/2]; its
// true average is the overlap-weighted sum of the texels that range touches.
// Adjacent texels are folded into ONE bilinear tap by placing that tap where
// the hardware interpolation weight equals the overlap ratio, so the tap count
// is ceil((ratio + 1) / 2) rather than `ratio`, and the weights are exact.
// Verified against an integral-image box resolve: mean and max error 0.000000.
//
// The footprint comes from the hardware derivatives of uv rather than from a
// constant buffer, so it cannot go out of step with the viewport the presenter
// actually set - and the full-screen triangle makes ddx/ddy exactly constant,
// so this costs one subtraction.
//
// ratio <= 1 is magnification: the footprint lies inside a single texel and the
// correct answer is the plain bilinear sample. Every internal scale 1
// configuration magnifies, so the shipped 1x look is unchanged by
// construction, and `gxPresentMaxTaps == 1` still reproduces the old single-tap
// blit exactly for the A/B arm.
#define GX_PRESENT_MAX_TAPS 8

struct GxAxisTaps
{
    float uv[GX_PRESENT_MAX_TAPS];
    float weight[GX_PRESENT_MAX_TAPS];
    int count;
};

GxAxisTaps GxBuildAxisTaps(float centre, float footprint, float dim, int limit)
{
    GxAxisTaps taps;
    taps.count = 1;
    taps.uv[0] = centre;
    taps.weight[0] = 1.0f;
    [unroll] for (int clear = 1; clear < GX_PRESENT_MAX_TAPS; ++clear)
    {
        taps.uv[clear] = centre;
        taps.weight[clear] = 0.0f;
    }
    if (footprint * dim <= 1.0f || limit <= 1) return taps;
    float low = (centre - footprint * 0.5f) * dim;
    float high = (centre + footprint * 0.5f) * dim;
    int first = int(floor(low));
    int last = int(ceil(high)) - 1;
    // Step s always consumes texels first+2s and first+2s+1, so the used count
    // is known in closed form and every array index below is a compile-time
    // constant. Writing them through a running counter instead forces the
    // compiler to unroll a [loop]-marked loop, which is an error (X3531).
    int pairs = min((last - first) / 2 + 1, limit);
    [unroll] for (int step = 0; step < GX_PRESENT_MAX_TAPS; ++step)
    {
        int texel = first + step * 2;
        float w0 = max(min(high, float(texel) + 1.0f) - max(low, float(texel)), 0.0f);
        float w1 = max(min(high, float(texel) + 2.0f) - max(low, float(texel) + 1.0f), 0.0f);
        float total = w0 + w1;
        // Bilinear position between texel and texel+1 whose interpolation
        // weight IS the overlap ratio, converted back to uv.
        taps.uv[step] = total > 0.0f ? (float(texel) + 0.5f + w1 / total) / dim : centre;
        taps.weight[step] = step < pairs ? total : 0.0f;
    }
    taps.count = max(pairs, 1);
    return taps;
}

bool GxPresentIsPlainSample(GxAxisTaps x, GxAxisTaps y)
{
    return x.count == 1 && y.count == 1 && x.weight[0] == 1.0f && y.weight[0] == 1.0f;
}

float4 PSPresentBlit(GxPresentVSOutput gxInput) : SV_Target
{
    uint width;
    uint height;
    gxPresentSource.GetDimensions(width, height);
    float2 footprint = float2(abs(ddx(gxInput.uv).x), abs(ddy(gxInput.uv).y));
    int limit = clamp(gxPresentMaxTaps, 1, GX_PRESENT_MAX_TAPS);
    GxAxisTaps tapsX = GxBuildAxisTaps(gxInput.uv.x, footprint.x, float(width), limit);
    GxAxisTaps tapsY = GxBuildAxisTaps(gxInput.uv.y, footprint.y, float(height), limit);
    if (GxPresentIsPlainSample(tapsX, tapsY))
    {
        return gxPresentSource.SampleLevel(gxPresentSampler, gxInput.uv, 0.0f);
    }
    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float total = 0.0f;
    [loop] for (int y = 0; y < tapsY.count; ++y)
    {
        [loop] for (int x = 0; x < tapsX.count; ++x)
        {
            float weight = tapsY.weight[y] * tapsX.weight[x];
            sum += gxPresentSource.SampleLevel(gxPresentSampler,
                                               float2(tapsX.uv[x], tapsY.uv[y]), 0.0f) *
                   weight;
            total += weight;
        }
    }
    return total > 0.0f ? sum / total
                        : gxPresentSource.SampleLevel(gxPresentSampler, gxInput.uv, 0.0f);
}

float4 PSPresentBlend(GxPresentVSOutput gxInput) : SV_Target
{
    uint width;
    uint height;
    gxPresentSource.GetDimensions(width, height);
    float2 footprint = float2(abs(ddx(gxInput.uv).x), abs(ddy(gxInput.uv).y));
    int limit = clamp(gxPresentMaxTaps, 1, GX_PRESENT_MAX_TAPS);
    GxAxisTaps tapsX = GxBuildAxisTaps(gxInput.uv.x, footprint.x, float(width), limit);
    GxAxisTaps tapsY = GxBuildAxisTaps(gxInput.uv.y, footprint.y, float(height), limit);
    if (GxPresentIsPlainSample(tapsX, tapsY))
    {
        float4 previous = gxPresentPrevious.SampleLevel(gxPresentSampler, gxInput.uv, 0.0f);
        float4 current = gxPresentSource.SampleLevel(gxPresentSampler, gxInput.uv, 0.0f);
        return lerp(previous, current, gxPresentAlpha);
    }
    float4 previousSum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 currentSum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float total = 0.0f;
    [loop] for (int y = 0; y < tapsY.count; ++y)
    {
        [loop] for (int x = 0; x < tapsX.count; ++x)
        {
            float weight = tapsY.weight[y] * tapsX.weight[x];
            float2 uv = float2(tapsX.uv[x], tapsY.uv[y]);
            previousSum += gxPresentPrevious.SampleLevel(gxPresentSampler, uv, 0.0f) * weight;
            currentSum += gxPresentSource.SampleLevel(gxPresentSampler, uv, 0.0f) * weight;
            total += weight;
        }
    }
    if (total <= 0.0f)
    {
        float4 previous = gxPresentPrevious.SampleLevel(gxPresentSampler, gxInput.uv, 0.0f);
        float4 current = gxPresentSource.SampleLevel(gxPresentSampler, gxInput.uv, 0.0f);
        return lerp(previous, current, gxPresentAlpha);
    }
    float inverse = 1.0f / total;
    return lerp(previousSum * inverse, currentSum * inverse, gxPresentAlpha);
}
)hlsl";
