// The native bloom ("glare") system's shaders.
//
// These are NOT a translation of the guest's Im2D draws. The original expressed
// its kernel in guest screen pixels against a 128x128 corner of a single 640x480
// framebuffer, because GX has one render target and one bilinear unit. Here the
// kernel is expressed in texels of the level render target itself, which is what
// makes it exact at any resolution and why no lattice can form: there are no
// sub-texels to comb.
//
// The mathematics are specified in gekko/bloom.h and validated against the CPU
// reference in runtime/tests/bloom_tests.cpp. Every formula below has a named
// counterpart there; if the two ever disagree the reference is right.
//
// Behavioural facts these encode, each measured from the PAL DOL and confirmed
// in a live command-stream capture:
//   * no luminance threshold and no bright-pass - what blooms is decided by
//     render-list membership, upstream of every shader here;
//   * the capture gain is a vertex colour in the original: 30/255 for level A
//     (trunc(0x20 * 0.95)) and the RAW 48/255 for level B's cascade arm, with
//     no 0.95 on that arm. Both observed as literal vertex colours 30,30,30,255
//     and 48,48,48,255 in the capture;
//   * taps per half are extra+1, from an unconditional centre tap;
//   * the border is ZERO, not clamp - the halo fades at the frame edge;
//   * every intermediate saturates at 8 bits. Level B's total gain is 4.7059,
//     so it clips above ~54/255 of its source. That clipping is the look, and
//     UNORM8 render targets reproduce it for free.

Texture2D<float4> g_source : register(t0, space0);
Texture2D<float4> g_level : register(t1, space0);
SamplerState g_point : register(s0, space1);
SamplerState g_linear : register(s1, space1);

cbuffer BloomConstants : register(b0, space2) {
    // Destination extent in texels, and its reciprocal.
    float4 dst_size;       // xy = w,h   zw = 1/w, 1/h
    // Source extent in texels, and its reciprocal.
    float4 src_size;       // xy = w,h   zw = 1/w, 1/h
    // Capture gain applied by the downsample pass.
    float gain;
    // Tap count actually used by a blur pass, including the centre tap.
    uint tap_count;
    // 0 = horizontal half, 1 = vertical half.
    uint tap_axis;
    uint bloom_pad0;
    // Tap offsets in texels of the SOURCE (= the level's own grid). Sixteen is
    // the generator's clamp; the shipped configuration uses 3 and 5.
    float4 taps[4];
};

struct FullscreenVertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// One oversized triangle, no vertex buffer, no input layout. Cheaper than two
// triangles and it has no diagonal seam.
FullscreenVertex VSBloom(uint id : SV_VertexID) {
    FullscreenVertex output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

// ---------------------------------------------------------------------------
// Capture: the exact continuous box over the destination texel's source
// footprint, with FRACTIONAL weights at the partial end texels.
//
// The original took a single bilinear probe at the footprint's centre. The box
// and the probe have the same first moment, so this changes variance and not
// brightness - which is the whole reason it is safe to change, and is asserted
// by the reference test. The fractional weights are not a nicety: the ratio is
// 11.16 at 1080p, and truncating the footprint to whole texels puts a beat
// pattern back into the image.
// ---------------------------------------------------------------------------
float4 PSBloomCapture(FullscreenVertex input) : SV_Target0 {
    float2 dst_index = floor(input.uv * dst_size.xy);
    float2 lo = dst_index * src_size.xy * dst_size.zw;
    float2 hi = (dst_index + 1.0) * src_size.xy * dst_size.zw;

    int2 first = int2(floor(lo));
    int2 last = int2(ceil(hi)) - 1;
    last = min(last, int2(src_size.xy) - 1);

    float4 sum = 0.0;
    float weight_total = 0.0;
    // Bounded so the compiler can unroll sensibly; the ratio is under 32 for
    // every supported combination of frame size and quality.
    [loop] for (int y = first.y; y <= last.y; ++y) {
        float wy = min(hi.y, float(y + 1)) - max(lo.y, float(y));
        if (wy <= 0.0) continue;
        [loop] for (int x = first.x; x <= last.x; ++x) {
            float wx = min(hi.x, float(x + 1)) - max(lo.x, float(x));
            if (wx <= 0.0) continue;
            float w = wx * wy;
            sum += g_source.Load(int3(x, y, 0)) * w;
            weight_total += w;
        }
    }
    if (weight_total <= 0.0) return 0.0;
    return saturate((sum / weight_total) * gain);
}

// ---------------------------------------------------------------------------
// One separable half. Unweighted, unnormalised, saturating, ZERO border.
//
// The offsets arrive in texels of the source level, so a tap is an exact texel
// fetch whenever the spread is integral - which it is in the shipped
// configuration - and a bilinear fetch otherwise, exactly as the hardware did.
// Out of range contributes nothing: the original shortened its quad rather than
// clamping the UV, which is the same thing and is visible at the frame edge.
// ---------------------------------------------------------------------------
float4 PSBloomBlur(FullscreenVertex input) : SV_Target0 {
    float2 texel = input.uv * src_size.xy;
    float4 sum = 0.0;
    [loop] for (uint i = 0; i < tap_count; ++i) {
        float d = taps[i >> 2][i & 3];
        float2 at = texel;
        if (tap_axis == 0u) {
            at.x -= d;
        } else {
            at.y -= d;
        }
        float limit = (tap_axis == 0u ? src_size.x : src_size.y) - 1.0;
        float position = (tap_axis == 0u ? at.x : at.y);
        // The centre of texel k is k + 0.5, so the level spans [0, extent-1] in
        // this index space. Anything outside is the zero border.
        if (position < -0.5 || position > limit + 0.5) continue;
        sum += g_level.SampleLevel(g_linear, at * src_size.zw, 0.0);
    }
    return saturate(sum);
}

// ---------------------------------------------------------------------------
// Merge: level A plus level B upsampled 2x, saturating.
//
// The upsample samples B at uv = (k + 0.5) / A_extent, the half-texel-correct
// convention. That places B's contribution on A's texel exactly - the alignment
// the original bought by displacing the entire bloom by half a level-A texel
// (+2.5 / +1.875 guest px). Here the alignment is free and the displacement is
// zero, which is a deliberate deviation: the original's offset was a side
// effect of the wrong constant, and under the new sizing rule it would grow
// with the frame instead of staying at the 2.5 px that shipped.
// ---------------------------------------------------------------------------
float4 PSBloomMerge(FullscreenVertex input) : SV_Target0 {
    float4 a = g_source.SampleLevel(g_point, input.uv, 0.0);
    float4 b = g_level.SampleLevel(g_linear, input.uv, 0.0);
    return saturate(a + b);
}

// ---------------------------------------------------------------------------
// Composite: the scene receives the glare source at unit weight plus the
// magnified level. Run with ONE/ONE blending onto the scene target, so the
// scene is never copied and never re-added - the original's full-screen scene
// snapshot and its add-back both disappear.
//
// The glare source is fetched 1:1 with no filtering; it is the same extent as
// the destination.
// ---------------------------------------------------------------------------
float4 PSBloomComposite(FullscreenVertex input) : SV_Target0 {
    float4 source = g_source.Load(int3(int2(input.uv * dst_size.xy), 0));
    float4 level = g_level.SampleLevel(g_linear, input.uv, 0.0);
    return saturate(source + level);
}
