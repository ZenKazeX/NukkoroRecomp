// The game's glare (docs/effects-2026-09-21/BLOOM_SPEC.md 2.13-2.14 and 6.4; docs/lift-20261006/R_DESIGN.md 2.5
// passes 2-4). The glare sources are drawn onto black over the scene's depth; then
//   A  = gain_a x box-downsampling of the sources to level A (96 W/H x 128 texels: one texel is H/96 x H/128 px),
//        summed over 1 + taps unweighted taps per axis, horizontal then vertical, zero outside the level;
//   B  = gain_b x the 2 x 2 box of A (level B is half of A), summed the same way;
//   A' = saturate(A + the bilinear magnification of B);
//   scene += sources + the bilinear magnification of A', saturating at white.
// Every level is UNORM8, so the clamp and the rounding at each step are the look: level B hard-clips into the flat
// wide plateau the artists tuned against (BLOOM_SPEC 2.13).
//
// Every result is `precise`: D3D lets a driver fuse a multiply and an add or not, and two compilations of the same
// shader (one process's and another's) did differ in the last bit, which an UNORM8 level turns into a step
// (R2_DRIVER.md 4.9, the determinism finding); precise forbids it for everything that feeds the value.
//
// Root constants (b0) per entry point:
//   PSGlareCapture    root0.x source, root1.xy source size, root1.zw level size, root2.x gain (float)
//   PSGlareBlur       root0.x level, root1.xy level size, root2.x taps, root2.y spread (float), root2.z axis (0 x, 1 y)
//   PSGlareBox        root0.x level A, root1.xy level A size, root2.x gain (float)
//   PSGlareMerge      root0.x level A, root0.y level B, root0.z sampler (linear, clamp), root1.xy level A size
//   PSGlareComposite  root0.x sources, root0.y level, root0.z second level (or none), root0.w sampler, root1.xy size

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

static const uint kNone = 0xFFFFFFFFu;

// A texel, zero outside the image.
float3 texel(Texture2D<float4> image, int2 p, int2 size)
{
    return all(p >= 0) && all(p < size) ? image.Load(int3(p, 0)).rgb : float3(0, 0, 0);
}

// Each level texel integrates the source over its exact footprint, partial texels weighted by their covered share.
float4 PSGlareCapture(ScreenVertex i) : SV_Target
{
    Texture2D<float4> source = ResourceDescriptorHeap[root0.x];
    const int2 size = int2(root1.xy);
    precise const float2 scale = float2(root1.xy) / float2(root1.zw);
    precise const float2 lo = floor(i.position.xy) * scale;
    precise const float2 hi = lo + scale;
    precise float3 sum = 0;
    for (int y = int(floor(lo.y)); y < int(ceil(hi.y)); ++y)
    {
        precise const float wy = min(hi.y, y + 1.0) - max(lo.y, float(y));
        for (int x = int(floor(lo.x)); x < int(ceil(hi.x)); ++x)
        {
            precise const float wx = min(hi.x, x + 1.0) - max(lo.x, float(x));
            sum += wx * wy * texel(source, int2(x, y), size);
        }
    }
    precise const float3 result = saturate(asfloat(root2.x) * sum / (scale.x * scale.y));
    return float4(result, 1.0);
}

// The capture of level A in exact arithmetic (GLARE_OPTIMIZATION.md G1): the footprint integral PSGlareCapture
// computes in float, computed on integers. A resolved source pixel is an 8-bit integer k; measured in units of
// 1 / L pixel (L the level's size along the axis), a pixel's span [x L, (x + 1) L) and a level texel's span
// [c S, (c + 1) S) (S the source's size) have integer ends, so every coverage weight is an integer and
//     level(c, r) = gain x sum over (x, y) of wx wy k(x, y) / (S_x S_y 255)
// is one integer sum (associative: the order of summation cannot change it) and one division. The float form rounded
// at every step, and which texels its rounding flipped depended on the order it summed in.
// The sum is below S_x S_y 255, which fits 32 bits up to 16.8 million source pixels (renderer.cpp falls back above).

static const uint kGlareRowPixels = 256;  // a CSGlareRows group's threads: one source pixel column each
static const uint kGlareRowLines = 16;    // and its source rows: one tile row (G2's 16 x 16 tiles)
groupshared uint g_glare_rows[kGlareRowLines * kGlareRowPixels];  // 16 KiB
groupshared uint g_glare_row_marked;

// CSGlareRows, one group per (root2.x level columns - as many as fit 256 pixels with their edges - one row of 16 x 16
// tiles): each thread reads one pixel column of the span over the 16 source rows into group memory (rows of an
// unmarked tile row are not read: 0), then each (column, row) pair sums its footprint, wx k, into partial[c, y].
//   root0.x resolved sources (SRV, UNORM8), root0.y the sources' tile marks (SRV, kNone: none, every tile counts as
//   marked), root0.z partial sums (UAV, uint4); root1.xy source size, root1.zw level size; root2.x columns per group.
[numthreads(256, 1, 1)]
void CSGlareRows(uint3 group : SV_GroupID, uint index : SV_GroupIndex)
{
    Texture2D<float4> sources = ResourceDescriptorHeap[root0.x];
    RWTexture2D<uint4> partial = ResourceDescriptorHeap[root0.z];
    const uint width = root1.x, height = root1.y, columns = root1.z;
    const uint y0 = group.y * kGlareRowLines;
    const uint lines = min(kGlareRowLines, height - y0);
    const uint per_group = root2.x;
    const uint c0 = group.x * per_group;
    const uint c1 = min(c0 + per_group, columns);
    const uint x0 = c0 * width / columns;                    // the span's first pixel
    const uint x1 = (c1 * width + columns - 1) / columns;    // one past its last
    const uint x = x0 + index;
    // G2: whether any tile of the span holds a source pixel; one thread per tile reads its mark.
    if (index == 0)
        g_glare_row_marked = root0.y == kNone ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();
    if (root0.y != kNone && index <= ((x1 - 1) >> 4) - (x0 >> 4))
    {
        Texture2D<uint> tiles = ResourceDescriptorHeap[root0.y];
        if (tiles.Load(int3((x0 >> 4) + index, group.y, 0)) != 0)
            InterlockedOr(g_glare_row_marked, 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    const bool marked = g_glare_row_marked != 0;
    for (uint r = 0; r < kGlareRowLines; ++r)
    {
        uint packed = 0;
        if (marked && r < lines && x < x1)
        {
            // An UNORM8 load is k / 255 to the nearest float: k back exactly.
            const uint3 k = uint3(round(sources.Load(int3(x, y0 + r, 0)).rgb * 255.0));
            packed = k.r | (k.g << 8) | (k.b << 16);
        }
        g_glare_rows[r * kGlareRowPixels + index] = packed;
    }
    GroupMemoryBarrierWithGroupSync();
    const uint count = c1 - c0;
    for (uint pair = index; pair < count * lines; pair += kGlareRowPixels)
    {
        const uint r = pair / count;
        const uint c = c0 + pair % count;
        uint3 sum = 0;
        if (marked)
        {
            const uint lo = c * width, hi = lo + width;      // the column's span, in 1 / columns pixel
            for (uint p = lo / columns; p < (hi + columns - 1) / columns; ++p)
            {
                const uint w = min((p + 1) * columns, hi) - max(p * columns, lo);
                const uint v = g_glare_rows[r * kGlareRowPixels + p - x0];
                sum += w * uint3(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF);
            }
        }
        partial[int2(c, y0 + r)] = uint4(sum, 0);
    }
}

// CSGlareColumns, one thread per level texel: the integer sum of the row sums over its rows, wy partial, then the one
// division, the gain, the clamp and the 8-bit store.
//   root0.x partial sums (SRV, uint4), root0.y level (UAV, UNORM8); root1.xy source size, root1.zw level size;
//   root2.x gain (float).
[numthreads(8, 8, 1)]
void CSGlareColumns(uint3 id : SV_DispatchThreadID)
{
    Texture2D<uint4> partial = ResourceDescriptorHeap[root0.x];
    RWTexture2D<unorm float4> level = ResourceDescriptorHeap[root0.y];
    const uint2 size = root1.xy, extent = root1.zw;
    if (id.x >= extent.x || id.y >= extent.y)
        return;
    const uint lo = id.y * size.y, hi = lo + size.y;         // the texel's rows, in 1 / extent.y pixel
    uint3 sum = 0;
    for (uint p = lo / extent.y; p < (hi + extent.y - 1) / extent.y; ++p)
    {
        const uint w = min((p + 1) * extent.y, hi) - max(p * extent.y, lo);
        sum += w * partial.Load(int3(id.x, p, 0)).rgb;
    }
    precise const float3 result = saturate(asfloat(root2.x) * (float3(sum) / float(size.x * size.y * 255u)));
    level[id.xy] = float4(result, 1.0);
}

// The sources' resolve on their marked tiles only (GLARE_OPTIMIZATION.md G2; the marks: glare_tiles.hlsli).
// VSGlareTiles draws instance i as tile (i % columns, i / columns), six vertices, its two triangles; an unmarked tile
// is degenerate and draws nothing - the cleared target keeps 0 there, which is what PSResolve would have written.
// PSResolve (utility.hlsl) shades it: the same bits as the full-screen resolve on every pixel it draws.
//   root0.x tile marks (SRV, uint), root0.y tile columns, root0.zw target size (pixels); PSResolve's root1.
ScreenVertex VSGlareTiles(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    Texture2D<uint> tiles = ResourceDescriptorHeap[root0.x];
    const uint2 tile = uint2(instance % root0.y, instance / root0.y);
    ScreenVertex o;
    o.uv = float2(0, 0);
    if (tiles.Load(int3(tile, 0)) == 0)
    {
        o.position = float4(0, 0, 0, 1);
        return o;
    }
    const uint2 corners[6] = {uint2(0, 0), uint2(1, 0), uint2(0, 1), uint2(1, 0), uint2(1, 1), uint2(0, 1)};
    const float2 p = min(float2((tile + corners[vertex]) * 16), float2(root0.zw));
    o.position = float4(p.x / float(root0.z) * 2.0 - 1.0, 1.0 - p.y / float(root0.w) * 2.0, 0.0, 1.0);
    return o;
}

// The tile marks cleared before a view's first glare-sources pass.   root0.x tile marks (UAV), root0.yz their size.
[numthreads(8, 8, 1)]
void CSGlareTilesClear(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= root0.y || id.y >= root0.z)
        return;
    RWTexture2D<uint> tiles = ResourceDescriptorHeap[root0.x];
    tiles[id.xy] = 0u;
}

// The tap set of the game's blur: 0, then for n = 0 .. taps - 1 the offset (n / 2 + 1) x spread with the sign
// alternating from +; destination k sums the level at k - offset (a fractional offset is a bilinear fetch).
float4 PSGlareBlur(ScreenVertex i) : SV_Target
{
    Texture2D<float4> level = ResourceDescriptorHeap[root0.x];
    const int2 size = int2(root1.xy);
    const uint taps = root2.x;
    const float spread = asfloat(root2.y);
    const int2 axis = root2.z == 0 ? int2(1, 0) : int2(0, 1);
    const int2 k = int2(i.position.xy);
    precise float3 sum = texel(level, k, size);
    float side = 1.0;
    for (uint n = 0; n < taps; ++n)
    {
        precise const float t = -float(n / 2 + 1) * side * spread;
        precise const float whole = floor(t);
        precise const float f = t - whole;
        const int2 p = k + axis * int(whole);
        sum += (1.0 - f) * texel(level, p, size) + f * texel(level, p + axis, size);
        side = -side;
    }
    precise const float3 result = saturate(sum);
    return float4(result, 1.0);
}

// Level B from level A: the mean of each 2 x 2 block, times the gain.
float4 PSGlareBox(ScreenVertex i) : SV_Target
{
    Texture2D<float4> level = ResourceDescriptorHeap[root0.x];
    const int2 size = int2(root1.xy);
    const int2 m = int2(i.position.xy) * 2;
    precise const float3 sum = texel(level, m, size) + texel(level, m + int2(1, 0), size) +
                               texel(level, m + int2(0, 1), size) + texel(level, m + int2(1, 1), size);
    precise const float3 result = saturate(asfloat(root2.x) * 0.25 * sum);
    return float4(result, 1.0);
}

// A' = A + B magnified to A's grid, centre on centre.
float4 PSGlareMerge(ScreenVertex i) : SV_Target
{
    Texture2D<float4> a = ResourceDescriptorHeap[root0.x];
    Texture2D<float4> b = ResourceDescriptorHeap[root0.y];
    SamplerState filter = SamplerDescriptorHeap[root0.z];
    precise const float2 uv = i.position.xy / float2(root1.xy);
    precise const float3 result = saturate(a.Load(int3(int2(i.position.xy), 0)).rgb + b.SampleLevel(filter, uv, 0.0).rgb);
    return float4(result, 1.0);
}

// What the glare adds to the scene (blended ONE / ONE, the scene's alpha kept): the sources at their own pixels and
// the level(s) magnified to the view.
float4 PSGlareComposite(ScreenVertex i) : SV_Target
{
    Texture2D<float4> sources = ResourceDescriptorHeap[root0.x];
    SamplerState filter = SamplerDescriptorHeap[root0.w];
    precise const float2 uv = i.position.xy / float2(root1.xy);
    precise float3 sum = root0.x != kNone ? sources.Load(int3(int2(i.position.xy), 0)).rgb : float3(0, 0, 0);
    if (root0.y != kNone)
    {
        Texture2D<float4> level = ResourceDescriptorHeap[root0.y];
        sum += level.SampleLevel(filter, uv, 0.0).rgb;
    }
    if (root0.z != kNone)
    {
        Texture2D<float4> level = ResourceDescriptorHeap[root0.z];
        sum += level.SampleLevel(filter, uv, 0.0).rgb;
    }
    return float4(sum, 0.0);
}

// The view's resolve with the glare composite folded in (B3a): each sample is what the composite's ONE / ONE blend
// would leave in the 16-bit UNORM scene - the sample plus the pixel's glare, clamped and rounded to the nearest of
// its 65536 steps - and the result is PSResolve's fixed-order mean of those samples. The alpha is the sample's (the
// blend keeps it). Measured against the plain composite (OPTIMIZATION_RESULTS.md, thirtieth increment): equal except
// where the GPU's own blend collapses an edge pixel's samples to one value (about 0.2% of pixels on Final Haunt), which
// this per-sample form does not; ~11 pixels per 2560 x 1440 frame differ by one 8-bit step. (Reading each step's float
// from a table of the GPU's own conversions instead cost 0.7 ms and changed nothing visible.)
// root0 as PSGlareComposite (sources, level, second level, sampler); root1.x the multisampled scene, root1.y its
// sample count; root2.xy the view size.
// The direct resolve (renderer.cpp; root3.y 1): the byte the compose path would store for this mean, from the device's
// measured thresholds (root3.z: R32_UINT 256 x 4, unorm_calibration.cpp). Row c, column k holds the first nonnegative
// float, as bits, whose byte through the R16 store -> point compose -> 8-bit store chain is >= k; bit order is numeric
// order for nonnegative floats. The calibration checks every threshold lies within 0.1 step of its ideal
// (k - 0.5) / 255, so with s = x * 255 + 0.5, k = floor(s), e = s - k:
//   0.125 <= e <= 0.875: x * 255 is within 0.375 of k, inside [T[k], T[k + 1]) - the byte is k, no table read;
//   e < 0.125: the byte is k - 1 or k, k exactly when x reaches T[k];
//   e > 0.875: k or k + 1, k + 1 exactly when x reaches T[k + 1].
// (s's own rounding is ~1e-5 of a step against the 0.025 margin.) Most pixels read nothing: the table reads per
// channel were what made a 2160p frame slower than the compose path at the idle core clock. Returned as k / 255,
// which the 8-bit store keeps as k (also checked by the calibration).
float direct_byte(Texture2D<uint> thresholds, float m, uint channel)
{
    const float x = m > 0.0 ? min(m, 1.0) : 0.0;
    const float s = x * 255.0 + 0.5;
    int k = min(int(s), 255);
    const float e = s - float(k);
    if (e < 0.125)
    {
        if (k > 0 && asuint(x) < thresholds.Load(int3(k, channel, 0)))
            --k;
    }
    else if (e > 0.875 && k < 255)
    {
        if (asuint(x) >= thresholds.Load(int3(k + 1, channel, 0)))
            ++k;
    }
    return float(k) / 255.0;
}

float4 direct_output(float4 mean)
{
    Texture2D<uint> thresholds = ResourceDescriptorHeap[root3.z];
    return float4(direct_byte(thresholds, mean.r, 0u), direct_byte(thresholds, mean.g, 1u),
                  direct_byte(thresholds, mean.b, 2u), direct_byte(thresholds, mean.a, 3u));
}

float stored_unorm16(float x)
{
    precise const float q = round(saturate(x) * 65535.0) / 65535.0;
    return q;
}

float4 PSResolveGlare(ScreenVertex i) : SV_Target
{
    Texture2DMS<float4> scene = ResourceDescriptorHeap[root1.x];
    Texture2D<float4> sources = ResourceDescriptorHeap[root0.x];
    SamplerState filter = SamplerDescriptorHeap[root0.w];
    precise const float2 uv = i.position.xy / float2(root2.xy);
    precise float3 glare = root0.x != kNone ? sources.Load(int3(int2(i.position.xy), 0)).rgb : float3(0, 0, 0);
    if (root0.y != kNone)
    {
        Texture2D<float4> level = ResourceDescriptorHeap[root0.y];
        glare += level.SampleLevel(filter, uv, 0.0).rgb;
    }
    if (root0.z != kNone)
    {
        Texture2D<float4> level = ResourceDescriptorHeap[root0.z];
        glare += level.SampleLevel(filter, uv, 0.0).rgb;
    }
    precise float4 sum = 0;
    for (uint s = 0; s < root1.y; ++s)
    {
        const float4 sample = scene.Load(int2(i.position.xy), s);
        precise const float3 blended = float3(stored_unorm16(sample.r + glare.r), stored_unorm16(sample.g + glare.g),
                                              stored_unorm16(sample.b + glare.b));
        sum += float4(blended, sample.a);
    }
    precise const float4 mean = sum / float(root1.y);
    // Diagnostics (B3a, renderer.cpp resolve_low_byte): the low byte of the 16-bit mean (1) or of sample 0..3 (2..5).
    if (root3.x >= 2)
    {
        const float4 one = scene.Load(int2(i.position.xy), root3.x - 2);
        const float3 blended = float3(stored_unorm16(one.r + glare.r), stored_unorm16(one.g + glare.g),
                                      stored_unorm16(one.b + glare.b));
        return float4(fmod(round(saturate(blended) * 65535.0), 256.0) / 255.0, 1.0);
    }
    if (root3.x == 1)
        return float4(fmod(round(saturate(mean.rgb) * 65535.0), 256.0) / 255.0, 1.0);
    // root3.y 1: straight into the 8-bit output - the byte the compose path would store (direct_output).
    if (root3.y != 0u)
        return direct_output(mean);
    return mean;
}
